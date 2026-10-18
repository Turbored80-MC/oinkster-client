#include "Oinkster/PacketSender.hpp"
#include "Oinkster/Capabilities.hpp"
#include "Oinkster/Diagnostics.hpp"
#include "Oinkster/HookManager.hpp"
#include "Oinkster/ModuleManager.hpp"
#include "Oinkster/RakNet.hpp"
#include "Oinkster/SafeMemory.hpp"
#include "Oinkster/SignatureScan.hpp"
#include "Oinkster/SupportedBuild.hpp"
#include "Oinkster/UnloadGuard.hpp"
#include <Windows.h>
#include <array>
#include <atomic>
#include <cstring>
#include <deque>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <sstream>

namespace Oinkster::PacketSender {
namespace {
using SendFn = void (*)(void* peer, const void* packet, int reliability, int compressibility);

constexpr std::size_t ConnectionPeer = 0x118;   // NetworkConnection -> NetworkPeer* (receivePacket reads it)
constexpr std::size_t SendPacketSlot = 1;
constexpr std::size_t MaxQueued = 32;
constexpr std::size_t MaxCommandBytes = 512;

SendFn originalSend{};
void* sendTarget{};
std::atomic<ModuleManager*> moduleManager{};
std::atomic_bool attached{};
std::atomic_uint32_t activeCallbacks{};
std::atomic<void*> serverPeer{};
std::atomic_bool peerValidated{};
std::atomic_bool havePending{};
std::atomic_uint64_t connectionGeneration{1};

std::mutex mutex;
std::optional<CommandTemplate> commandTemplate;
std::optional<FormTemplate> formTemplate;
std::deque<std::vector<std::uint8_t>> pending;

struct CallbackGuard final {
    CallbackGuard() noexcept { activeCallbacks.fetch_add(1, std::memory_order_acq_rel); }
    ~CallbackGuard() { activeCallbacks.fetch_sub(1, std::memory_order_acq_rel); }
    CallbackGuard(const CallbackGuard&) = delete;
    CallbackGuard& operator=(const CallbackGuard&) = delete;
};

// MSVC std::string as the game lays it out (see ReplayPacketBridge).
struct StringHeader {
    char buffer[16];
    std::size_t size;
    std::size_t capacity;
};

std::filesystem::path TemplatePath() {
    wchar_t localAppData[MAX_PATH]{};
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return {};
    return std::filesystem::path(localAppData) / "Oinkster" / "Client" / "packet-templates.txt";
}

std::string BuildName() {
    std::string name;
    for (const wchar_t c : std::wstring_view(SupportedBuild::PackageVersion)) name.push_back(static_cast<char>(c));
    return name;
}

void SaveTemplatesLocked() noexcept {
    try {
        const auto path = TemplatePath();
        if (path.empty()) return;
        std::filesystem::create_directories(path.parent_path());
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << EncodeTemplates(commandTemplate, formTemplate, BuildName());
    } catch (...) {}
}

void LoadTemplates() noexcept {
    try {
        const auto path = TemplatePath();
        if (path.empty() || !std::filesystem::exists(path)) return;
        std::ifstream input(path, std::ios::binary);
        std::stringstream text;
        text << input.rdbuf();
        std::scoped_lock lock(mutex);
        DecodeTemplates(text.str(), BuildName(), commandTemplate, formTemplate);
    } catch (...) {}
}

void MarkAvailable() noexcept {
    if (peerValidated.exchange(true)) return;
    if (auto* modules = moduleManager.load(std::memory_order_acquire)) {
        try { modules->SetCapability(Capability::PacketSend, {CapabilityState::Available, {}}); } catch (...) {}
    }
    std::scoped_lock lock(mutex);
    Diagnostics::LogLine("info", "packet_send_validated", std::format("server peer validated; command template {}, form template {}",
        commandTemplate ? "known" : "not learned yet", formTemplate ? "known" : "not learned yet"));
}

void Learn(std::span<const std::uint8_t> packet) noexcept {
    try {
        RakNet::Reader reader(packet);
        const auto id = reader.VarU32() & 0x3FFU;
        if (id != CommandRequestId && id != ModalFormResponseId) return;
        std::scoped_lock lock(mutex);
        if (id == CommandRequestId) {
            auto learned = LearnCommandTemplate(packet);
            if (!learned || (commandTemplate && commandTemplate->header == learned->header && commandTemplate->suffix == learned->suffix)) return;
            commandTemplate = std::move(learned);
            Diagnostics::LogLine("info", "packet_send_template", std::format("learned CommandRequest layout: {} header byte(s), {} byte suffix",
                commandTemplate->header.size(), commandTemplate->suffix.size()));
        } else {
            auto learned = LearnFormTemplate(packet);
            if (!learned || (formTemplate && formTemplate->beforeResponse == learned->beforeResponse && formTemplate->suffix == learned->suffix)) return;
            formTemplate = std::move(learned);
            Diagnostics::LogLine("info", "packet_send_template", std::format("learned ModalFormResponse layout: {} byte prefix, {} byte suffix",
                formTemplate->beforeResponse.size(), formTemplate->suffix.size()));
        }
        SaveTemplatesLocked();
    } catch (...) {}
}

// Watches the player's own packets on the server connection; never changes them.
void HookedSend(void* peer, const void* packet, int reliability, int compressibility) {
    const CallbackGuard callback;
    if (attached.load(std::memory_order_acquire) && peer && peer == serverPeer.load(std::memory_order_acquire) && packet) {
        StringHeader header{};
        if (SafeMemory::ReadField(packet, 0, header) && header.size > 1 && header.size <= 4096 && header.capacity >= header.size) {
            std::array<std::uint8_t, 4096> bytes{};
            const void* data = header.buffer;
            if (header.capacity >= 16) std::memcpy(&data, header.buffer, sizeof(data));
            if (data && SafeMemory::CopyGuarded(bytes.data(), data, header.size)) Learn(std::span(bytes.data(), header.size));
        }
    }
    if (originalSend) originalSend(peer, packet, reliability, compressibility);
}

void SendNow(void* peer, std::vector<std::uint8_t> bytes) noexcept {
    // Our own buffer behind a game-shaped string: heap form (capacity >= 16),
    // NUL-terminated. sendPacket copies it into the batch and never frees it.
    const std::size_t size = bytes.size();
    bytes.resize(std::max<std::size_t>(size + 1, 17), 0);
    StringHeader header{};
    const void* data = bytes.data();
    std::memcpy(header.buffer, &data, sizeof(data));
    header.size = size;
    header.capacity = bytes.size() - 1;
    if (originalSend) originalSend(peer, &header, 1, 0);
}

bool Queue(std::vector<std::uint8_t> bytes, std::uint64_t expectedGeneration) {
    std::scoped_lock lock(mutex);
    if (!attached.load(std::memory_order_acquire) || !peerValidated.load(std::memory_order_acquire)
        || connectionGeneration.load(std::memory_order_acquire) != expectedGeneration || pending.size() >= MaxQueued) return false;
    pending.push_back(std::move(bytes));
    havePending.store(true, std::memory_order_release);
    return true;
}

void AppendVarU32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    do {
        std::uint8_t byte = value & 0x7FU;
        value >>= 7U;
        if (value != 0) byte |= 0x80U;
        out.push_back(byte);
    } while (value != 0);
}

std::string Hex(const std::vector<std::uint8_t>& bytes) {
    std::string text;
    for (const auto byte : bytes) text += std::format("{:02x}", byte);
    return text;
}

std::optional<std::vector<std::uint8_t>> Unhex(std::string_view text) {
    if (text.size() % 2 != 0 || text.size() > 8192) return std::nullopt;
    const auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::vector<std::uint8_t> bytes;
    for (std::size_t at = 0; at < text.size(); at += 2) {
        const int high = nibble(text[at]);
        const int low = nibble(text[at + 1]);
        if (high < 0 || low < 0) return std::nullopt;
        bytes.push_back(static_cast<std::uint8_t>(high * 16 + low));
    }
    return bytes;
}

std::vector<std::string_view> Split(std::string_view text, char separator) {
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    for (std::size_t at = 0; at <= text.size(); ++at) {
        if (at == text.size() || text[at] == separator) {
            parts.push_back(text.substr(start, at - start));
            start = at + 1;
        }
    }
    return parts;
}
}

std::optional<CommandTemplate> LearnCommandTemplate(std::span<const std::uint8_t> packet) {
    RakNet::Reader reader(packet);
    const auto header = reader.VarU32();
    if (!reader.Ok() || (header & 0x3FFU) != CommandRequestId) return std::nullopt;
    const std::size_t headerLength = reader.Position();
    const auto length = reader.VarU32();
    if (!reader.Ok() || length == 0 || length > MaxCommandBytes || length > reader.Remaining()) return std::nullopt;
    const auto command = reader.Bytes(length);
    if (command.empty() || command[0] != '/') return std::nullopt;
    const auto rest = reader.Rest();
    // Origin, IsInternal and Version: small, and never empty.
    if (rest.empty() || rest.size() > 256) return std::nullopt;
    CommandTemplate learned;
    learned.header.assign(packet.begin(), packet.begin() + static_cast<std::ptrdiff_t>(headerLength));
    learned.suffix.assign(rest.begin(), rest.end());
    return learned;
}

std::vector<std::uint8_t> BuildCommand(const CommandTemplate& learned, std::string_view command) {
    std::vector<std::uint8_t> bytes = learned.header;
    AppendVarU32(bytes, static_cast<std::uint32_t>(command.size()));
    bytes.insert(bytes.end(), command.begin(), command.end());
    bytes.insert(bytes.end(), learned.suffix.begin(), learned.suffix.end());
    return bytes;
}

std::optional<FormTemplate> LearnFormTemplate(std::span<const std::uint8_t> packet) {
    RakNet::Reader reader(packet);
    const auto header = reader.VarU32();
    if (!reader.Ok() || (header & 0x3FFU) != ModalFormResponseId) return std::nullopt;
    const std::size_t headerLength = reader.Position();
    (void)reader.VarU32();  // form id
    const std::size_t afterId = reader.Position();
    // An answered form: "has response" then the response string.
    if (reader.U8() != 1) return std::nullopt;
    const std::size_t stringAt = reader.Position();
    const auto length = reader.VarU32();
    if (!reader.Ok() || length == 0 || length > 4096 || length > reader.Remaining()) return std::nullopt;
    reader.Skip(length);
    const auto rest = reader.Rest();
    if (!reader.Ok() || rest.size() > 64) return std::nullopt;
    FormTemplate learned;
    learned.header.assign(packet.begin(), packet.begin() + static_cast<std::ptrdiff_t>(headerLength));
    learned.beforeResponse.assign(packet.begin() + static_cast<std::ptrdiff_t>(afterId), packet.begin() + static_cast<std::ptrdiff_t>(stringAt));
    learned.suffix.assign(rest.begin(), rest.end());
    return learned;
}

std::vector<std::uint8_t> BuildFormResponse(const FormTemplate& learned, std::uint32_t formId, std::string_view response) {
    std::vector<std::uint8_t> bytes = learned.header;
    AppendVarU32(bytes, formId);
    bytes.insert(bytes.end(), learned.beforeResponse.begin(), learned.beforeResponse.end());
    AppendVarU32(bytes, static_cast<std::uint32_t>(response.size()));
    bytes.insert(bytes.end(), response.begin(), response.end());
    bytes.insert(bytes.end(), learned.suffix.begin(), learned.suffix.end());
    return bytes;
}

std::string EncodeTemplates(const std::optional<CommandTemplate>& command, const std::optional<FormTemplate>& form, std::string_view build) {
    std::string text = std::format("build={}\n", build);
    if (command) text += std::format("command={}:{}\n", Hex(command->header), Hex(command->suffix));
    if (form) text += std::format("form={}:{}:{}\n", Hex(form->header), Hex(form->beforeResponse), Hex(form->suffix));
    return text;
}

void DecodeTemplates(std::string_view text, std::string_view build, std::optional<CommandTemplate>& command,
    std::optional<FormTemplate>& form) {
    command.reset();
    form.reset();
    bool sameBuild = false;
    for (auto line : Split(text, '\n')) {
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        const auto equals = line.find('=');
        if (equals == std::string_view::npos) continue;
        const auto key = line.substr(0, equals);
        const auto value = line.substr(equals + 1);
        if (key == "build") sameBuild = value == build;
        if (!sameBuild) continue;
        const auto parts = Split(value, ':');
        if (key == "command" && parts.size() == 2) {
            auto header = Unhex(parts[0]);
            auto suffix = Unhex(parts[1]);
            if (header && suffix && !header->empty() && !suffix->empty()) command = CommandTemplate{std::move(*header), std::move(*suffix)};
        } else if (key == "form" && parts.size() == 3) {
            auto header = Unhex(parts[0]);
            auto before = Unhex(parts[1]);
            auto suffix = Unhex(parts[2]);
            if (header && before && suffix && !header->empty()) form = FormTemplate{std::move(*header), std::move(*before), std::move(*suffix)};
        }
    }
}

bool Install(ModuleManager& modules, std::wstring& detail) noexcept {
    const std::wstring build(SupportedBuild::DisplayVersion);
    // BatchedNetworkPeer::sendPacket: the r9 -> ebx, rdx -> rdi, rcx -> rsi
    // shuffle and the batch stream at +0x18 pin it (unique in the image).
    constexpr std::array<std::string_view, 1> signatures{
        "55 41 57 41 56 41 55 41 54 56 57 53 48 83 EC 78 48 8D 6C 24 70 48 C7 45 00 FE FF FF FF "
        "44 89 CB 48 89 D7 48 89 CE 48 83 C1 18 4C 8B 72 10 48 83 7A 18 10",
    };
    const auto selection = SignatureScan::FindUniqueInText(signatures);
    Diagnostics::LogLine(selection.outcome == SignatureScan::Outcome::Unique ? "info" : "warning", "packet_send_signature",
        std::string(SignatureScan::OutcomeName(selection.outcome)));
    const auto fail = [&](std::wstring reason) {
        detail = std::move(reason);
        try { modules.SetCapability(Capability::PacketSend, {CapabilityState::Unavailable, detail}); } catch (...) {}
        return false;
    };
    if (selection.outcome != SignatureScan::Outcome::Unique) {
        return fail(L"The packet-send signature was not unique in Minecraft " + build + L".");
    }
    void* target = reinterpret_cast<void*>(selection.address);
    if (!HookManager::Create(target, reinterpret_cast<void*>(&HookedSend), reinterpret_cast<void**>(&originalSend))) {
        return fail(L"The packet-send hook could not be installed.");
    }
    sendTarget = target;
    LoadTemplates();
    peerValidated.store(false);
    serverPeer.store(nullptr);
    connectionGeneration.fetch_add(1, std::memory_order_acq_rel);
    moduleManager.store(&modules, std::memory_order_release);
    attached.store(true, std::memory_order_release);
    try {
        modules.SetCapability(Capability::PacketSend, {CapabilityState::Pending,
            L"Waiting for a server connection in Minecraft " + build + L"."});
    } catch (...) {}
    detail = L"Packet sender attached (BatchedNetworkPeer::sendPacket).";
    return true;
}

void Remove() noexcept {
    attached.store(false, std::memory_order_release);
    serverPeer.store(nullptr);
    peerValidated.store(false, std::memory_order_release);
    connectionGeneration.fetch_add(1, std::memory_order_acq_rel);
    if (auto* modules = moduleManager.exchange(nullptr, std::memory_order_acq_rel)) {
        try { modules->SetCapability(Capability::PacketSend, {CapabilityState::Unavailable, L"The packet sender was detached."}); } catch (...) {}
    }
    {
        std::scoped_lock lock(mutex);
        pending.clear();
        havePending.store(false);
    }
    if (!sendTarget) return;
    (void)HookManager::Disable(sendTarget);
    for (unsigned attempt = 0; activeCallbacks.load(std::memory_order_acquire) != 0 && attempt < 200; ++attempt) Sleep(5);
    if (activeCallbacks.load(std::memory_order_acquire) != 0) {
        Diagnostics::LogLine("warning", "packet_send_remove", "a thread is still inside the packet-send detour; leaving its trampoline installed");
        UnloadGuard::Block("packet send trampoline still had a live thread at detach");
        return;
    }
    (void)HookManager::Remove(sendTarget);
    sendTarget = nullptr;
    originalSend = nullptr;
}

void Poll(void* serverConnection) noexcept {
    if (!attached.load(std::memory_order_acquire) || !sendTarget) return;
    if (!serverConnection) {
        const bool hadPeer = serverPeer.exchange(nullptr, std::memory_order_acq_rel) != nullptr;
        const bool wasValidated = peerValidated.exchange(false, std::memory_order_acq_rel);
        bool hadPending{};
        {
            std::scoped_lock lock(mutex);
            hadPending = !pending.empty();
            pending.clear();
            havePending.store(false, std::memory_order_release);
        }
        if (!hadPeer && !wasValidated && !hadPending) return;
        connectionGeneration.fetch_add(1, std::memory_order_acq_rel);
        if (auto* modules = moduleManager.load(std::memory_order_acquire)) {
            try { modules->SetCapability(Capability::PacketSend,
                {CapabilityState::Pending, L"Waiting for a server connection."}); } catch (...) {}
        }
        Diagnostics::LogLine("info", "packet_send_connection", "server connection ended; discarded queued packets");
        return;
    }
    void* peer{};
    void* vtable{};
    if (!SafeMemory::ReadField(serverConnection, ConnectionPeer, peer) || !peer || !SafeMemory::ReadField(peer, 0, vtable) || !vtable
        || SafeMemory::ReadVirtualSlot(peer, SendPacketSlot) != sendTarget) {
        Poll(nullptr);
        return;
    }
    void* previousPeer = serverPeer.exchange(peer, std::memory_order_acq_rel);
    if (previousPeer && previousPeer != peer) {
        std::scoped_lock lock(mutex);
        connectionGeneration.fetch_add(1, std::memory_order_acq_rel);
        pending.clear();
        havePending.store(false, std::memory_order_release);
        peerValidated.store(false, std::memory_order_release);
        Diagnostics::LogLine("info", "packet_send_connection", "server peer changed; discarded queued packets from the previous connection");
    }
    MarkAvailable();
    if (!havePending.load(std::memory_order_acquire)) return;
    std::deque<std::vector<std::uint8_t>> batch;
    {
        std::scoped_lock lock(mutex);
        batch.swap(pending);
        havePending.store(false, std::memory_order_release);
    }
    for (auto& packet : batch) SendNow(peer, std::move(packet));
}

bool CanSendCommands() noexcept {
    std::scoped_lock lock(mutex);
    return peerValidated.load() && commandTemplate.has_value();
}

bool CanSendFormResponses() noexcept {
    std::scoped_lock lock(mutex);
    return peerValidated.load() && formTemplate.has_value();
}

bool QueueCommand(std::string_view command) noexcept {
    try {
        if (command.empty() || command.size() > MaxCommandBytes || command[0] != '/') return false;
        std::vector<std::uint8_t> bytes;
        std::uint64_t generation{};
        {
            std::scoped_lock lock(mutex);
            if (!peerValidated.load() || !commandTemplate) return false;
            generation = connectionGeneration.load(std::memory_order_acquire);
            bytes = BuildCommand(*commandTemplate, command);
        }
        if (!Queue(std::move(bytes), generation)) return false;
        Diagnostics::LogLine("info", "packet_send_command", std::string(command.substr(0, command.find(' '))));
        return true;
    } catch (...) {
        return false;
    }
}

bool QueuePacket(std::vector<std::uint8_t> packet) noexcept {
    try {
        if (packet.empty() || packet.size() > 512 * 1024 || !peerValidated.load()) return false;
        const auto generation = connectionGeneration.load(std::memory_order_acquire);
        RakNet::Reader reader(packet);
        const auto id = reader.VarU32() & 0x3FFU;
        if (!reader.Ok() || !Queue(std::move(packet), generation)) return false;
        Diagnostics::LogLine("info", "packet_send_packet", std::format("packet id {}", id));
        return true;
    } catch (...) {
        return false;
    }
}

bool QueueFormResponse(std::uint32_t formId, std::string_view response) noexcept {
    try {
        if (response.empty() || response.size() > 4096) return false;
        std::vector<std::uint8_t> bytes;
        std::uint64_t generation{};
        {
            std::scoped_lock lock(mutex);
            if (!peerValidated.load() || !formTemplate) return false;
            generation = connectionGeneration.load(std::memory_order_acquire);
            bytes = BuildFormResponse(*formTemplate, formId, response);
        }
        if (!Queue(std::move(bytes), generation)) return false;
        Diagnostics::LogLine("info", "packet_send_form", std::format("form {}", formId));
        return true;
    } catch (...) {
        return false;
    }
}

}
