#include "Oinkster/ReplayPacketBridge.hpp"

#include "Oinkster/Diagnostics.hpp"
#include "Oinkster/HookManager.hpp"
#include "Oinkster/ModuleManager.hpp"
#include "Oinkster/PacketSender.hpp"
#include "Oinkster/ReplayBlockPack.hpp"
#include "Oinkster/ReplayCapture.hpp"
#include "Oinkster/ReplayInjector.hpp"
#include "Oinkster/SafeMemory.hpp"
#include "Oinkster/SignatureScan.hpp"
#include "Oinkster/SupportedBuild.hpp"
#include "Oinkster/UnloadGuard.hpp"
#include <Windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <format>
#include <mutex>
#include <optional>
#include <string_view>
#include <vector>

namespace Oinkster::ReplayPacketBridge {
namespace {

// NetworkConnection::receivePacket on 1.26.5203 (RVA 0xD935A0), clang ABI:
//   DataStatus (NetworkConnection* this, std::string& out, time_point* at)
// Re-derived from the image's only arbitrary-packet decode loop (RVA
// 0x16AADF0, NetworkHandler's per-connection event pump), which is also this
// function's only caller (Tools/FindCallXrefs.py):
//   call receivePacket(connection, &handler->mReceiveBuffer /*+0x128*/, ...)
//   test eax, eax / jne <no more data>            ; 0 = a packet was returned
//   ReadOnlyBinaryStream{vftable RVA 0xE75BD20, view = {buffer.data, buffer.size}, readPointer = 0}
//   header = varuint from offset 0; id = header & 0x3FF -> MinecraftPackets::createPacket(id)
// So on a 0 return `out` holds exactly one decoded packet, header included,
// after the connection's decryption and decompression. The function moves the
// string out of a std::deque<std::string> at connection+0x148.
using ReceiveFn = int (*)(void* connection, void* out, void* at);

constexpr int DataAvailable = 0;
// Bedrock rejects anything near this size long before it is decoded.
constexpr std::size_t MaxPacketBytes = 32U * 1024U * 1024U;
constexpr std::uint32_t LoggedPacketsPerSession = 24;

ReceiveFn originalReceive{};
void* receiveTarget{};
std::atomic<ModuleManager*> moduleManager{};
std::atomic_bool attached{};
std::atomic_bool validated{};
std::atomic_uint32_t activeCallbacks{};
std::mutex classifierMutex;
ReplayCapture::ConnectionClassifier classifier;
std::uint32_t loggedSession{};
std::uint32_t loggedInSession{};
// The connection the server's packets arrive on (the client's connection to
// the server); PacketSender writes through its peer.
std::atomic<void*> serverConnection{};

struct CallbackGuard final {
    CallbackGuard() noexcept { activeCallbacks.fetch_add(1, std::memory_order_acq_rel); }
    ~CallbackGuard() { activeCallbacks.fetch_sub(1, std::memory_order_acq_rel); }
    CallbackGuard(const CallbackGuard&) = delete;
    CallbackGuard& operator=(const CallbackGuard&) = delete;
};

// MSVC std::string header: 16-byte SSO buffer or heap pointer, size, capacity.
struct StringHeader {
    char buffer[16];
    std::size_t size;
    std::size_t capacity;
};

// Reads at most the first five bytes - enough for the header varint - so a
// packet on a connection that will not be captured is never copied whole.
bool ReadPrefix(const StringHeader& header, std::array<std::uint8_t, 5>& prefix, std::size_t& prefixLength) noexcept {
    prefixLength = std::min<std::size_t>(header.size, prefix.size());
    if (header.capacity < 16) {
        std::memcpy(prefix.data(), header.buffer, prefixLength);
        return true;
    }
    const void* heap{};
    std::memcpy(&heap, header.buffer, sizeof(heap));
    return heap && SafeMemory::CopyGuarded(prefix.data(), heap, prefixLength);
}

struct Observed {
    std::uint32_t id{};
    bool sessionStart{};
    // A packet the client sent, seen on the local world's server side.
    bool fromClient{};
};

// Classifies the packet the game just received; for server packets, also
// publishes it to an enabled recorder. nullopt for client packets and
// anything unreadable.
std::optional<Observed> Observe(void* connection, const void* out) noexcept {
    auto* modules = moduleManager.load(std::memory_order_acquire);
    if (!modules || !connection || !out) return std::nullopt;
    try {
        StringHeader header{};
        if (!SafeMemory::ReadField(out, 0, header)) return std::nullopt;
        if (header.size == 0 || header.size > MaxPacketBytes || header.capacity < 15 || header.size > header.capacity) return std::nullopt;
        std::array<std::uint8_t, 5> prefix{};
        std::size_t prefixLength{};
        if (!ReadPrefix(header, prefix, prefixLength)) return std::nullopt;
        const auto parsed = ReplayCapture::ParseHeader(std::span(prefix.data(), prefixLength));
        if (!parsed) return std::nullopt;

        ReplayCapture::Classification classification;
        bool logPacket = false;
        {
            std::scoped_lock lock(classifierMutex);
            classification = classifier.Observe(reinterpret_cast<std::uintptr_t>(connection), parsed->id);
            if (classification.direction == ReplayCapture::Direction::ServerToClient) {
                if (classification.session != loggedSession) {
                    loggedSession = classification.session;
                    loggedInSession = 0;
                }
                logPacket = loggedInSession++ < LoggedPacketsPerSession;
            }
            if (parsed->id == ReplayCapture::PacketId::Disconnect) {
                if (serverConnection.load(std::memory_order_acquire) == connection) serverConnection.store(nullptr, std::memory_order_release);
                classifier.Forget(reinterpret_cast<std::uintptr_t>(connection));
            }
        }
        if (classification.direction == ReplayCapture::Direction::ClientToServer) return Observed{parsed->id, false, true};
        if (classification.direction != ReplayCapture::Direction::ServerToClient) return std::nullopt;
        const Observed observed{parsed->id, classification.sessionStart};
        serverConnection.store(connection, std::memory_order_release);

        if (!validated.exchange(true)) {
            Diagnostics::LogLine("info", "replay_capture_validated", std::format("first server packet id={}", parsed->id));
        }
        if (classification.sessionStart) {
            Diagnostics::LogLine("info", "replay_capture_session", std::format("session={} joinedFromStart={} firstId={}",
                classification.session, classification.joinedFromStart, parsed->id));
        }
        // Ids and sizes only: packet contents never reach the log.
        if (logPacket) {
            Diagnostics::LogLine("info", "replay_capture_sample", std::format("session={} id={} bytes={}",
                classification.session, parsed->id, header.size));
        }
        // Copy the whole packet only when someone records or reads it - and
        // never while a replay is being watched.
        if (ReplayInjector::Running()) return observed;
        bool wanted = !modules->EnabledModulesOf<IReplayControlModule>().empty();
        if (!wanted) {
            for (const auto* observer : modules->EnabledModulesOf<IPacketObserverModule>()) {
                if (observer->WantsPacket(parsed->id)) { wanted = true; break; }
            }
        }
        if (!wanted) return observed;
        ReplayPacketEvent event{SteadyClock::now(), classification.session, classification.sessionStart,
            classification.joinedFromStart, parsed->id, {}};
        if (!SafeMemory::ReadMsvcString(out, 0, event.bytes, MaxPacketBytes)) return observed;
        modules->Publish(event);
        return observed;
    } catch (...) {
        return std::nullopt;
    }
}

// Replay injection hands the game a recorded packet by pointing its receive
// string at our own buffer for the duration of one decode, rather than
// allocating into the string: the game frees that buffer with its own
// allocator, which we cannot match. The decode loop reads the string as a
// view, then calls receivePacket again - and the first thing that call does
// is put the game's own string state back, before the original runs.
//
// Only the decode-loop thread touches these.
struct LentString {
    void* string{};
    StringHeader saved{};
};
LentString lent;
// Mirrors lent.string for Remove(), which runs on another thread.
std::atomic_bool lentOut{};
std::vector<std::uint8_t> lentBuffer;
// After this many recorded packets in a row, report "no more data" once so
// rebuilding a world (thousands of chunk packets) spreads over several ticks.
constexpr std::uint32_t MaxInjectedPerPoll = 256;
std::uint32_t injectedInPoll{};

void RestoreLentString(void* out) noexcept {
    if (!lent.string || lent.string != out) return;
    std::memcpy(out, &lent.saved, sizeof(StringHeader));
    lent.string = nullptr;
    lentOut.store(false, std::memory_order_release);
}

void LendString(void* out, std::vector<std::uint8_t> packet) noexcept {
    std::memcpy(&lent.saved, out, sizeof(StringHeader));
    lent.string = out;
    lentBuffer = std::move(packet);
    const std::size_t size = lentBuffer.size();
    // NUL-terminated, and past the 15-byte small-string capacity so the
    // string reads its data through the heap pointer.
    lentBuffer.resize(std::max<std::size_t>(size + 1, 17), 0);
    StringHeader header{};
    const void* data = lentBuffer.data();
    std::memcpy(header.buffer, &data, sizeof(data));
    header.size = size;
    header.capacity = lentBuffer.size() - 1;
    std::memcpy(out, &header, sizeof(StringHeader));
    lentOut.store(true, std::memory_order_release);
}

int HookedReceive(void* connection, void* out, void* at) {
    const CallbackGuard callback;
    const auto* original = originalReceive;
    if (!original) return 1;
    RestoreLentString(out);
    const auto key = reinterpret_cast<std::uintptr_t>(connection);
    // Queued client packets go out on the game's network thread, between
    // its own reads of the server connection - never during a replay.
    if (connection == serverConnection.load(std::memory_order_acquire) && !ReplayInjector::Running()) PacketSender::Poll(connection);
    for (;;) {
        const int status = original(connection, out, at);
        if (!attached.load(std::memory_order_acquire)) return status;
        if (status == DataAvailable) {
            const auto observed = Observe(connection, out);
            if (observed && observed->fromClient) {
                if (!ReplayInjector::Running()) return status;
                // Pack-download requests need their payload so the recording
                // can answer them. Other packets need no copy: once playback
                // starts, Controller drops them before the throwaway local
                // server can reject recorded actor/world identifiers.
                if (ReplayInjector::WantsClientPacket(observed->id)) {
                    std::string bytes;
                    if (!SafeMemory::ReadMsvcString(out, 0, bytes, MaxPacketBytes)) return status;
                    const std::span packet(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size());
                    if (ReplayInjector::OnClientPacket(packet) == ReplayInjector::Verdict::Pass) return status;
                } else if (ReplayInjector::OnClientPacket({}) == ReplayInjector::Verdict::Pass) {
                    return status;
                }
                continue;
            }
            // Diagnostics: whether the replay world loaded the recorded
            // server's blocks (its own pack list, before it is swapped).
            if (observed && observed->id == ReplayCapture::PacketId::ResourcePacksInfo && ReplayInjector::Running()) {
                std::string info;
                if (SafeMemory::ReadMsvcString(out, 0, info, MaxPacketBytes)) {
                    Diagnostics::LogLine("info", "replay_world_packs", std::format("local world pack list bytes={} hasReplayBlocks={}",
                        info.size(), info.find(ReplayBlockPack::PackUuid) != std::string::npos));
                }
            }
            if (!observed || ReplayInjector::OnServerPacket(key, observed->id, observed->sessionStart)
                    == ReplayInjector::Verdict::Pass) {
                return status;
            }
            // The local world's own packet is replaced by the recording.
            if (observed->id == ReplayCapture::PacketId::Disconnect) {
                std::string kick;
                if (SafeMemory::ReadMsvcString(out, 0, kick, 4096)) {
                    ReplayInjector::NoteDroppedDisconnect(std::span(reinterpret_cast<const std::uint8_t*>(kick.data()), kick.size()));
                }
            }
            continue;
        }
        if (injectedInPoll >= MaxInjectedPerPoll) {
            injectedInPoll = 0;
            return status;
        }
        std::vector<std::uint8_t> packet;
        if (!ReplayInjector::NextPacket(key, packet)) {
            injectedInPoll = 0;
            return status;
        }
        LendString(out, std::move(packet));
        ++injectedInPoll;
        return DataAvailable;
    }
}

}

bool Install(ModuleManager& modules, std::wstring& detail) noexcept {
    const std::wstring build(SupportedBuild::DisplayVersion);
    // Clang prologue (1.26.5203): frame sizes and field offsets wildcarded;
    // the r8 -> rbx, rdx -> rdi, rcx -> r13 argument shuffle followed by the
    // ReadOnlyBinaryStream vftable load pins the function. Verified unique
    // with Tools/SurveySignatures.py.
    constexpr std::array<std::string_view, 1> signatures{
        "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC ? ? ? ? 48 8D AC 24 ? ? ? ? 0F 29 B5 ? ? ? ? "
        "48 C7 85 ? ? ? ? FE FF FF FF 4C 89 C3 48 89 D7 49 89 CD 48 8D 81 ? ? ? ? 48 89 45 ? "
        "4C 8D 35 ? ? ? ? 0F 57 F6",
    };
    const auto selection = SignatureScan::FindUniqueInText(signatures);
    Diagnostics::LogLine(selection.outcome == SignatureScan::Outcome::Unique ? "info" : "warning",
        "replay_capture_signature", std::string(SignatureScan::OutcomeName(selection.outcome)));
    const auto fail = [&](std::wstring reason) {
        detail = std::move(reason);
        try { modules.SetCapability(Capability::PacketCapture, {CapabilityState::Unavailable, detail}); } catch (...) {}
        return false;
    };
    if (selection.outcome != SignatureScan::Outcome::Unique) {
        return fail(selection.outcome == SignatureScan::Outcome::Ambiguous
            ? L"The packet-receive signature matched more than once in Minecraft " + build + L"; refusing to hook an ambiguous target."
            : L"The packet-receive signature was not found in Minecraft " + build + L".");
    }
    void* target = reinterpret_cast<void*>(selection.address);
    if (!HookManager::Create(target, reinterpret_cast<void*>(&HookedReceive), reinterpret_cast<void**>(&originalReceive))) {
        return fail(L"The packet-receive hook could not be installed.");
    }
    receiveTarget = target;
    validated.store(false);
    {
        std::scoped_lock lock(classifierMutex);
        classifier.Reset();
        loggedSession = 0;
        loggedInSession = 0;
    }
    moduleManager.store(&modules, std::memory_order_release);
    attached.store(true, std::memory_order_release);
    // The hook target and ABI have already been validated at this point. Make
    // Replay Mod available before the first world packet arrives, otherwise a
    // persisted enable request cannot observe the join sequence it needs.
    try { modules.SetCapability(Capability::PacketCapture, {CapabilityState::Available, {}}); } catch (...) {}
    detail = L"Packet capture attached (NetworkConnection::receivePacket, server packets only).";
    return true;
}

void Remove() noexcept {
    attached.store(false, std::memory_order_release);
    serverConnection.store(nullptr, std::memory_order_release);
    if (auto* modules = moduleManager.exchange(nullptr, std::memory_order_acq_rel)) {
        try { modules->SetCapability(Capability::PacketCapture, {CapabilityState::Unavailable, L"Packet capture was detached."}); } catch (...) {}
    }
    if (!receiveTarget) return;
    // Unhooked while the game still reads a recorded packet from our buffer,
    // its next receivePacket would free that buffer with the game's allocator.
    // The decode loop returns it on its next call, normally within a tick.
    for (unsigned attempt = 0; lentOut.load(std::memory_order_acquire) && attempt < 200; ++attempt) Sleep(5);
    if (lentOut.load(std::memory_order_acquire)) {
        Diagnostics::LogLine("warning", "replay_capture_remove", "the game still holds a replay packet buffer; leaving the packet hook installed");
        UnloadGuard::Block("replay packet buffer still lent to the game at detach");
        return;
    }
    (void)HookManager::Disable(receiveTarget);
    for (unsigned attempt = 0; activeCallbacks.load(std::memory_order_acquire) != 0 && attempt < 200; ++attempt) Sleep(5);
    if (activeCallbacks.load(std::memory_order_acquire) != 0) {
        Diagnostics::LogLine("warning", "replay_capture_remove", "a thread is still inside the packet-receive detour; leaving its trampoline installed");
        UnloadGuard::Block("packet capture trampoline still had a live thread at detach");
        return;
    }
    (void)HookManager::Remove(receiveTarget);
    receiveTarget = nullptr;
    originalReceive = nullptr;
}

}
