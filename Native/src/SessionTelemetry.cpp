#include <WinSock2.h>
#include <WS2tcpip.h>
#include "Oinkster/SessionTelemetry.hpp"
#include "Oinkster/Diagnostics.hpp"
#include "Oinkster/HookManager.hpp"
#include "Oinkster/ModuleManager.hpp"
#include "Oinkster/SafeMemory.hpp"
#include "Oinkster/ServerPolicy.hpp"
#include "Oinkster/SignatureScan.hpp"
#include "Oinkster/SupportedBuild.hpp"
#include "Oinkster/UnloadGuard.hpp"
#include "Oinkster/Modules/StatusLogic.hpp"
#include "Oinkster/Modules/TelemetryLogic.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <format>
#include <mutex>
#include <string>
#include <vector>

namespace Oinkster::SessionTelemetry {
namespace {

// RakNet::RakPeer on 1.26.5203, every field confirmed against the build's own
// code:
//  - Receive is RakPeerInterface vtable slot 25 (RVA 0x1990E50): it locks the
//    packet queue at +0x8DD8 and pops from the ring at +0x8E00.
//  - GetLastPing (slot 40, RVA 0x1994490) and GetIndexFromSystemAddress (RVA
//    0x1998360) walk remoteSystemList at +0x248 for maximumNumberOfPeers
//    (+0x14) entries of 0x2698 bytes; GetMaximumIncomingConnections (slot 9)
//    returns the u32 at +0x18; slot 18 refuses to work while bit 0 of +0x10
//    (endThreads) is set.
//  - RemoteSystemStruct: isActive (bool) at +0x0 (the lookup at RVA 0x1994AFB
//    compares it with 1), the SystemAddress sockaddr at +0x8 (family u16 2 /
//    0x17, IPv4 at +0xC, IPv6 at +0x10), and a 5-entry {u16 pingTime, ...}
//    ring at +0x25F0 with a 0x10 stride (GetLastPing reads the newest entry
//    via the write index at +0x2640, wrapping to +0x2630).
constexpr std::size_t RakPeer_EndThreads = 0x10;
constexpr std::size_t RakPeer_MaximumNumberOfPeers = 0x14;
constexpr std::size_t RakPeer_MaximumIncomingConnections = 0x18;
constexpr std::size_t RakPeer_RemoteSystemList = 0x248;
constexpr std::size_t RemoteSystem_Stride = 0x2698;
constexpr std::size_t RemoteSystem_IsActive = 0x0;
constexpr std::size_t RemoteSystem_AddressFamily = 0x8;
constexpr std::size_t RemoteSystem_Ipv4 = 0xC;
constexpr std::size_t RemoteSystem_Ipv6 = 0x10;
constexpr std::size_t RemoteSystem_PingRing = 0x25F0;
constexpr std::size_t PingRingStride = 0x10;
constexpr std::size_t PingRingEntries = 5;
constexpr std::uint16_t FamilyIpv4 = 2;
constexpr std::uint16_t FamilyIpv6 = 0x17;
// A client peer has a handful of slots; a server peer far more. Anything past
// this is a misread, not a peer.
constexpr std::uint32_t MaxPlausiblePeers = 1024;

// World identity, from the telemetry builder at RVA 0x120DC60 (the event that
// sends "WorldName"): Actor+0x1D8 is the actor's ILevel*, ILevel vtable slot
// 173 (+0x568) is getLevelData(), and LevelData+0x2A8 is mLevelName - the
// std::string LevelData::getTagData (RVA 0x5FC3F0) fills from "LevelName".
// On a server that is the world name the server sent at join.
constexpr std::size_t Actor_Level = 0x1D8;
constexpr std::size_t ILevel_GetLevelDataSlot = 0x568 / sizeof(void*);
constexpr std::size_t LevelData_LevelName = 0x2A8;

constexpr auto PeerSampleInterval = std::chrono::milliseconds(250);
constexpr auto PublishInterval = std::chrono::seconds(1);
// A client peer that has not been sampled for this long has gone away (the
// player left the server); its ping and session kind no longer apply.
constexpr auto ClientFreshness = std::chrono::seconds(3);
constexpr auto SampleLogInterval = std::chrono::seconds(10);

using ReceiveFn = void* (*)(void*);
using GetLevelDataFn = void* (*)(void*);
using GetAddrInfoAFn = INT(WSAAPI*)(PCSTR, PCSTR, const ADDRINFOA*, PADDRINFOA*);
using GetAddrInfoWFn = INT(WSAAPI*)(PCWSTR, PCWSTR, const ADDRINFOW*, PADDRINFOW*);

ReceiveFn originalReceive{};
void* receiveTarget{};
GetAddrInfoAFn originalGetAddrInfoA{};
GetAddrInfoWFn originalGetAddrInfoW{};
void* getAddrInfoATarget{};
void* getAddrInfoWTarget{};
std::atomic<ModuleManager*> moduleManager{};
std::atomic_bool attached{};
std::atomic_uint32_t activeCallbacks{};

std::uintptr_t imageBase{};
std::uintptr_t textBegin{};
std::uintptr_t textEnd{};

// Written by the Receive detour (network thread), read by Observe.
constexpr std::uint32_t NoPing = 0xFFFFFFFFU;
std::atomic<std::int64_t> clientSampledAt{};
std::atomic_uint32_t clientPing{NoPing};
std::atomic_uint8_t clientKind{static_cast<std::uint8_t>(SessionKind::None)};
std::atomic_uint8_t clientNetwork{static_cast<std::uint8_t>(ServerNetwork::Local)};

// DNS results are retained only as short-lived in-memory endpoint mappings.
// Neither hostname nor address is logged, published, or persisted.
struct ResolvedEndpoint {
    int family{};
    std::array<std::uint8_t, 16> address{};
    ServerNetwork network{ServerNetwork::UnknownRemote};
    SteadyTime expiresAt{};
};
constexpr auto ResolutionLifetime = std::chrono::minutes(10);
constexpr std::size_t MaxResolvedEndpoints = 64;
std::mutex resolutionsMutex;
std::vector<ResolvedEndpoint> resolutions;

// Detour-side bookkeeping, shared by every thread that drains a peer.
struct PeerRecord { const void* peer{}; SteadyTime sampledAt{}; bool logged{}; };
std::mutex peersMutex;
std::array<PeerRecord, 4> peers{};

// Render-thread only.
SteadyTime lastPublished{};
SteadyTime lastSampleLogged{};
bool networkValidated{};
bool serverValidated{};
bool levelSlotLogged{};
std::wstring lastName;

struct CallbackGuard final {
    CallbackGuard() noexcept { activeCallbacks.fetch_add(1, std::memory_order_acq_rel); }
    ~CallbackGuard() { activeCallbacks.fetch_sub(1, std::memory_order_acq_rel); }
    CallbackGuard(const CallbackGuard&) = delete;
    CallbackGuard& operator=(const CallbackGuard&) = delete;
};

std::int64_t Ticks(SteadyTime time) noexcept { return time.time_since_epoch().count(); }

bool ResolveText() noexcept {
    const auto* base = reinterpret_cast<const std::uint8_t*>(GetModuleHandleW(nullptr));
    IMAGE_DOS_HEADER dos{};
    if (!base || !SafeMemory::ReadField(base, 0, dos) || dos.e_magic != IMAGE_DOS_SIGNATURE) return false;
    IMAGE_NT_HEADERS64 nt{};
    if (!SafeMemory::ReadField(base, static_cast<std::size_t>(dos.e_lfanew), nt) || nt.Signature != IMAGE_NT_SIGNATURE) return false;
    const std::size_t sections = static_cast<std::size_t>(dos.e_lfanew) + offsetof(IMAGE_NT_HEADERS64, OptionalHeader)
        + nt.FileHeader.SizeOfOptionalHeader;
    for (std::size_t index = 0; index < nt.FileHeader.NumberOfSections; ++index) {
        IMAGE_SECTION_HEADER section{};
        if (!SafeMemory::ReadField(base, sections + index * sizeof(section), section)) return false;
        if (std::memcmp(section.Name, ".text", 6) == 0) {
            imageBase = reinterpret_cast<std::uintptr_t>(base);
            textBegin = reinterpret_cast<std::uintptr_t>(base) + section.VirtualAddress;
            textEnd = textBegin + section.Misc.VirtualSize;
            return true;
        }
    }
    return false;
}

bool InText(const void* address) noexcept {
    const auto value = reinterpret_cast<std::uintptr_t>(address);
    return value >= textBegin && value < textEnd;
}

template<typename AddressInfo>
void RememberResolution(ServerNetwork network, const AddressInfo* results) noexcept {
    if (network == ServerNetwork::UnknownRemote || !results) return;
    try {
        const auto now = SteadyClock::now();
        std::scoped_lock lock(resolutionsMutex);
        std::erase_if(resolutions, [&](const ResolvedEndpoint& value) { return value.expiresAt <= now; });
        for (auto* item = results; item; item = item->ai_next) {
            ResolvedEndpoint endpoint;
            endpoint.family = item->ai_family;
            endpoint.network = network;
            endpoint.expiresAt = now + ResolutionLifetime;
            if (item->ai_family == AF_INET && item->ai_addrlen >= sizeof(sockaddr_in)) {
                const auto* address = reinterpret_cast<const sockaddr_in*>(item->ai_addr);
                std::memcpy(endpoint.address.data(), &address->sin_addr, 4);
            } else if (item->ai_family == AF_INET6 && item->ai_addrlen >= sizeof(sockaddr_in6)) {
                const auto* address = reinterpret_cast<const sockaddr_in6*>(item->ai_addr);
                std::memcpy(endpoint.address.data(), &address->sin6_addr, 16);
            } else {
                continue;
            }
            const auto same = [&](const ResolvedEndpoint& value) {
                return value.family == endpoint.family && value.address == endpoint.address && value.network == endpoint.network;
            };
            if (std::ranges::find_if(resolutions, same) == resolutions.end()) {
                if (resolutions.size() >= MaxResolvedEndpoints) resolutions.erase(resolutions.begin());
                resolutions.push_back(endpoint);
            }
        }
    } catch (...) {}
}

INT WSAAPI HookedGetAddrInfoA(PCSTR node, PCSTR service, const ADDRINFOA* hints, PADDRINFOA* result) {
    const CallbackGuard callback;
    const INT status = originalGetAddrInfoA ? originalGetAddrInfoA(node, service, hints, result) : EAI_FAIL;
    if (status == 0 && node && result) RememberResolution(ServerPolicy::ClassifyHostname(node), *result);
    return status;
}

INT WSAAPI HookedGetAddrInfoW(PCWSTR node, PCWSTR service, const ADDRINFOW* hints, PADDRINFOW* result) {
    const CallbackGuard callback;
    const INT status = originalGetAddrInfoW ? originalGetAddrInfoW(node, service, hints, result) : EAI_FAIL;
    if (status == 0 && node && result) RememberResolution(ServerPolicy::ClassifyHostname(node), *result);
    return status;
}

bool InstallResolverHooks() noexcept {
    const HMODULE ws2 = GetModuleHandleW(L"ws2_32.dll");
    if (!ws2) return false;
    getAddrInfoATarget = reinterpret_cast<void*>(GetProcAddress(ws2, "getaddrinfo"));
    getAddrInfoWTarget = reinterpret_cast<void*>(GetProcAddress(ws2, "GetAddrInfoW"));
    bool installedAny{};
    if (getAddrInfoATarget && HookManager::Create(getAddrInfoATarget, reinterpret_cast<void*>(&HookedGetAddrInfoA),
        reinterpret_cast<void**>(&originalGetAddrInfoA))) installedAny = true;
    else getAddrInfoATarget = nullptr;
    if (getAddrInfoWTarget && HookManager::Create(getAddrInfoWTarget, reinterpret_cast<void*>(&HookedGetAddrInfoW),
        reinterpret_cast<void**>(&originalGetAddrInfoW))) installedAny = true;
    else getAddrInfoWTarget = nullptr;
    return installedAny;
}

ServerNetwork MatchKnownRemote(int family, const std::array<std::uint8_t, 16>& address, SteadyTime now) noexcept {
    try {
        std::scoped_lock lock(resolutionsMutex);
        std::erase_if(resolutions, [&](const ResolvedEndpoint& value) { return value.expiresAt <= now; });
        ServerNetwork match = ServerNetwork::UnknownRemote;
        for (const auto& value : resolutions) {
            if (value.family != family || value.address != address) continue;
            if (match != ServerNetwork::UnknownRemote && match != value.network) return ServerNetwork::UnknownRemote;
            match = value.network;
        }
        return match;
    } catch (...) { return ServerNetwork::UnknownRemote; }
}

// POD-only so it needs no unwinding (a requirement for __try).
bool CallGetterGuarded(GetLevelDataFn getter, void* self, void*& result) noexcept {
    __try {
        result = getter(self);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Throttles sampling per peer; returns false while the peer was sampled
// recently. `firstSeen` is true exactly once per peer, for a diagnostic line.
bool ShouldSample(const void* peer, SteadyTime now, bool& firstSeen) {
    std::scoped_lock lock(peersMutex);
    PeerRecord* slot = nullptr;
    for (auto& record : peers) {
        if (record.peer == peer) { slot = &record; break; }
    }
    if (!slot) {
        slot = &*std::ranges::min_element(peers, {}, &PeerRecord::sampledAt);
        *slot = {peer, {}, false};
    }
    if (slot->sampledAt != SteadyTime{} && now - slot->sampledAt < PeerSampleInterval) return false;
    slot->sampledAt = now;
    firstSeen = !slot->logged;
    slot->logged = true;
    return true;
}

struct RemoteClassification {
    SessionKind kind{SessionKind::Server};
    ServerNetwork network{ServerNetwork::UnknownRemote};
};

RemoteClassification ClassifyRemote(const std::uint8_t* remote, SteadyTime now) noexcept {
    std::uint16_t family{};
    if (!SafeMemory::ReadField(remote, RemoteSystem_AddressFamily, family)) return {};
    if (family == FamilyIpv4) {
        std::array<std::uint8_t, 4> address{};
        if (SafeMemory::ReadField(remote, RemoteSystem_Ipv4, address)) {
            const auto kind = Modules::ClassifyRemoteIpv4(address);
            if (kind != SessionKind::Server) return {kind, ServerNetwork::Local};
            std::array<std::uint8_t, 16> key{};
            std::ranges::copy(address, key.begin());
            return {kind, MatchKnownRemote(AF_INET, key, now)};
        }
    } else if (family == FamilyIpv6) {
        std::array<std::uint8_t, 16> address{};
        if (SafeMemory::ReadField(remote, RemoteSystem_Ipv6, address)) {
            const auto kind = Modules::ClassifyRemoteIpv6(address);
            return {kind, kind == SessionKind::Server ? MatchKnownRemote(AF_INET6, address, now) : ServerNetwork::Local};
        }
    }
    return {};
}

void SamplePeer(const void* peer) noexcept {
    try {
        const auto now = SteadyClock::now();
        bool firstSeen{};
        if (!ShouldSample(peer, now, firstSeen)) return;

        std::uint8_t endThreads{};
        std::uint32_t maxPeers{};
        std::uint32_t maxIncoming{};
        const void* remotes{};
        if (!SafeMemory::ReadField(peer, RakPeer_EndThreads, endThreads)
            || !SafeMemory::ReadField(peer, RakPeer_MaximumNumberOfPeers, maxPeers)
            || !SafeMemory::ReadField(peer, RakPeer_MaximumIncomingConnections, maxIncoming)
            || !SafeMemory::ReadField(peer, RakPeer_RemoteSystemList, remotes)) {
            return;
        }
        if ((endThreads & 1) != 0 || !remotes || maxPeers == 0 || maxPeers > MaxPlausiblePeers || maxIncoming > maxPeers) return;

        std::uint32_t active{};
        const std::uint8_t* connected = nullptr;
        for (std::uint32_t index = 0; index < maxPeers; ++index) {
            const auto* remote = static_cast<const std::uint8_t*>(remotes) + static_cast<std::size_t>(index) * RemoteSystem_Stride;
            std::uint8_t isActive{};
            if (!SafeMemory::ReadField(remote, RemoteSystem_IsActive, isActive)) return;
            if (isActive == 1) {
                ++active;
                connected = remote;
            }
        }

        if (firstSeen) {
            Diagnostics::LogLine("info", "session_peer", std::format(
                "maxPeers={} maxIncoming={} active={}", maxPeers, maxIncoming, active));
        }
        // A peer that accepts connections is the local host (an open-to-LAN
        // world or the integrated server), not the client's link to a server.
        if (maxIncoming != 0 || active != 1 || !connected) return;

        std::array<std::uint16_t, PingRingEntries> ring{};
        for (std::size_t entry = 0; entry < ring.size(); ++entry) {
            if (!SafeMemory::ReadField(connected, RemoteSystem_PingRing + entry * PingRingStride, ring[entry])) return;
        }
        const auto ping = Modules::AveragePing(ring);
        clientPing.store(ping.value_or(NoPing), std::memory_order_relaxed);
        const auto classification = ClassifyRemote(connected, now);
        clientKind.store(static_cast<std::uint8_t>(classification.kind), std::memory_order_relaxed);
        clientNetwork.store(static_cast<std::uint8_t>(classification.network), std::memory_order_relaxed);
        clientSampledAt.store(Ticks(now), std::memory_order_release);
        if (auto* modules = moduleManager.load(std::memory_order_acquire)) {
            ServerPolicy::Apply(*modules, classification.kind, classification.network);
        }
    } catch (...) {}
}

void* HookedReceive(void* peer) {
    const CallbackGuard callback;
    void* packet = originalReceive ? originalReceive(peer) : nullptr;
    if (attached.load(std::memory_order_acquire) && peer) SamplePeer(peer);
    return packet;
}

bool ReadLevelName(const void* actor, std::string& name) noexcept {
    void* level{};
    if (!SafeMemory::ReadField(actor, Actor_Level, level) || !level) return false;
    void* slot = SafeMemory::ReadVirtualSlot(level, ILevel_GetLevelDataSlot);
    // Only ever call into Minecraft's own code.
    if (!slot || !InText(slot)) return false;
    if (!levelSlotLogged) {
        levelSlotLogged = true;
        std::array<std::uint8_t, 16> bytes{};
        if (SafeMemory::ReadField(slot, 0, bytes)) {
            std::string hex;
            for (const auto byte : bytes) hex += std::format("{:02X} ", byte);
            Diagnostics::LogLine("info", "session_level_data_slot", std::format(
                "rva=0x{:X} bytes={}", reinterpret_cast<std::uintptr_t>(slot) - imageBase, hex));
        }
    }
    void* levelData{};
    if (!CallGetterGuarded(reinterpret_cast<GetLevelDataFn>(slot), level, levelData) || !levelData) return false;
    try {
        return SafeMemory::ReadMsvcString(levelData, LevelData_LevelName, name, 256);
    } catch (...) {
        return false;
    }
}

void Promote(ModuleManager& modules, Capability capability, bool& validated, std::string_view what) noexcept {
    if (validated) return;
    validated = true;
    try {
        modules.SetCapability(capability, {CapabilityState::Available, {}});
        Diagnostics::LogLine("info", "session_validated", std::string(what));
    } catch (...) {}
}

}

bool Install(ModuleManager& modules, std::wstring& detail) noexcept {
    const std::wstring build(SupportedBuild::DisplayVersion);
    try {
        modules.SetCapability(Capability::ServerState, {CapabilityState::Pending,
            L"Waiting for the first validated LevelData read in Minecraft " + build + L"."});
    } catch (...) {}
    ServerPolicy::Apply(modules, SessionKind::None, ServerNetwork::Local);

    // RakNet::RakPeer::Receive, clang prologue (1.26.5203): the frame sizes
    // are wildcarded; the vtable call at +0x88 (IsActive) and the plugin-list
    // count at +0x358 pin the function. Verified unique in .text.
    constexpr std::array<std::string_view, 1> signatures{
        "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC ? ? ? ? 48 8D AC 24 ? ? ? ? 48 C7 85 ? ? ? ? FE FF FF FF "
        "48 89 CE 48 8B 01 48 8B 80 88 00 00 00 FF 15 ? ? ? ? 84 C0 0F 84 ? ? ? ? 83 BE 58 03 00 00 00",
    };
    const auto selection = SignatureScan::FindUniqueInText(signatures);
    Diagnostics::LogLine(selection.outcome == SignatureScan::Outcome::Unique ? "info" : "warning",
        "session_telemetry_signature", std::string(SignatureScan::OutcomeName(selection.outcome)));
    const auto fail = [&](std::wstring reason) {
        detail = std::move(reason);
        try {
            modules.SetCapability(Capability::NetworkMetrics, {CapabilityState::Unavailable, detail});
            ServerPolicy::Apply(modules, SessionKind::Server, ServerNetwork::UnknownRemote);
            modules.SetCapability(Capability::DcPreventPolicy,
                {CapabilityState::Unavailable, L"Server policy telemetry is unavailable, so DC Prevent is blocked."});
            modules.SetCapability(Capability::CpsCapPolicy,
                {CapabilityState::Unavailable, L"Server policy telemetry is unavailable, so CPS Cap is blocked."});
        } catch (...) {}
        moduleManager.store(&modules, std::memory_order_release);
        return false;
    };
    if (!ResolveText()) return fail(L"Minecraft's code section could not be located.");
    if (selection.outcome != SignatureScan::Outcome::Unique) {
        return fail(selection.outcome == SignatureScan::Outcome::Ambiguous
            ? L"The RakPeer::Receive signature matched more than once in Minecraft " + build + L"; refusing to hook an ambiguous target."
            : L"The RakPeer::Receive signature was not found in Minecraft " + build + L".");
    }

    void* target = reinterpret_cast<void*>(selection.address);
    if (!HookManager::Create(target, reinterpret_cast<void*>(&HookedReceive), reinterpret_cast<void**>(&originalReceive))) {
        return fail(L"The network-telemetry hook could not be installed.");
    }
    receiveTarget = target;
    const bool resolverInstalled = InstallResolverHooks();
    Diagnostics::LogLine(resolverInstalled ? "info" : "warning", "server_policy_resolver",
        resolverInstalled ? "Privacy-preserving known-network resolver hooks installed"
                          : "Resolver hooks unavailable; every remote server will fail closed");
    clientSampledAt.store(0);
    clientPing.store(NoPing);
    clientKind.store(static_cast<std::uint8_t>(SessionKind::None));
    clientNetwork.store(static_cast<std::uint8_t>(ServerNetwork::Local));
    {
        std::scoped_lock lock(peersMutex);
        peers = {};
    }
    {
        std::scoped_lock lock(resolutionsMutex);
        resolutions.clear();
    }
    lastPublished = {};
    lastSampleLogged = {};
    networkValidated = false;
    serverValidated = false;
    levelSlotLogged = false;
    lastName.clear();
    try {
        modules.SetCapability(Capability::NetworkMetrics, {CapabilityState::Pending,
            L"Waiting for a connected RakNet peer in Minecraft " + build + L"."});
    } catch (...) {}
    moduleManager.store(&modules, std::memory_order_release);
    attached.store(true, std::memory_order_release);
    detail = L"Session telemetry attached (RakPeer::Receive).";
    return true;
}

void Remove() noexcept {
    // Same disable -> drain -> remove order as WorldTelemetry::Remove.
    attached.store(false, std::memory_order_release);
    auto* modules = moduleManager.exchange(nullptr, std::memory_order_acq_rel);
    if (modules) {
        try {
            modules->SetCapability(Capability::NetworkMetrics, {CapabilityState::Unavailable, L"Session telemetry was detached."});
            modules->SetCapability(Capability::ServerState, {CapabilityState::Unavailable, L"Session telemetry was detached."});
            ServerPolicy::Apply(*modules, SessionKind::None, ServerNetwork::Local);
            modules->SetCapability(Capability::DcPreventPolicy, {CapabilityState::Unavailable, L"Server policy telemetry was detached."});
            modules->SetCapability(Capability::CpsCapPolicy, {CapabilityState::Unavailable, L"Server policy telemetry was detached."});
        } catch (...) {}
    }
    if (receiveTarget) (void)HookManager::Disable(receiveTarget);
    if (getAddrInfoATarget) (void)HookManager::Disable(getAddrInfoATarget);
    if (getAddrInfoWTarget) (void)HookManager::Disable(getAddrInfoWTarget);
    for (unsigned attempt = 0; activeCallbacks.load(std::memory_order_acquire) != 0 && attempt < 200; ++attempt) Sleep(5);
    if (activeCallbacks.load(std::memory_order_acquire) != 0) {
        Diagnostics::LogLine("warning", "session_telemetry_remove",
            "network thread still inside the RakPeer::Receive detour; leaving its trampoline installed");
        UnloadGuard::Block("session telemetry trampoline still had a live thread at detach");
        return;
    }
    if (receiveTarget) (void)HookManager::Remove(receiveTarget);
    if (getAddrInfoATarget) (void)HookManager::Remove(getAddrInfoATarget);
    if (getAddrInfoWTarget) (void)HookManager::Remove(getAddrInfoWTarget);
    receiveTarget = nullptr;
    originalReceive = nullptr;
    getAddrInfoATarget = nullptr;
    getAddrInfoWTarget = nullptr;
    originalGetAddrInfoA = nullptr;
    originalGetAddrInfoW = nullptr;
    {
        std::scoped_lock lock(resolutionsMutex);
        resolutions.clear();
    }
}

void Observe(const void* actor, SteadyTime now) noexcept {
    auto* modules = moduleManager.load(std::memory_order_acquire);
    if (!modules) return;

    const auto sampledAt = clientSampledAt.load(std::memory_order_acquire);
    const bool hadClientSample = sampledAt != 0;
    const bool connected = sampledAt != 0 && Ticks(now) - sampledAt < std::chrono::duration_cast<SteadyClock::duration>(ClientFreshness).count();
    const auto ping = clientPing.load(std::memory_order_relaxed);
    auto kind = connected ? static_cast<SessionKind>(clientKind.load(std::memory_order_relaxed)) : SessionKind::None;
    auto network = connected ? static_cast<ServerNetwork>(clientNetwork.load(std::memory_order_relaxed)) : ServerNetwork::Local;

    // A missing actor means there is no validated local world. Clear a stale
    // remote decision immediately on the next observation instead of treating
    // missing network telemetry as permission to filter input.
    if (!actor && (ServerPolicy::CurrentSessionKind() != kind || ServerPolicy::CurrentNetwork() != network)) {
        try { ServerPolicy::Apply(*modules, kind, network); } catch (...) {}
    }
    if (!actor && !connected) clientSampledAt.store(0, std::memory_order_release);
    if (!actor || now - lastPublished < PublishInterval) return;
    lastPublished = now;

    try {
        if (connected && ping != NoPing && attached.load(std::memory_order_acquire)) {
            Promote(*modules, Capability::NetworkMetrics, networkValidated, "RakPeer ping ring");
            modules->Publish(NetworkMetrics{now, ping});
        }

        std::string rawName;
        if (ReadLevelName(actor, rawName)) {
            if (!connected && !hadClientSample) {
                kind = SessionKind::Singleplayer;
                network = ServerNetwork::Local;
            }
            ServerState state{now, kind, Modules::CleanWorldName(rawName), network};
            ServerPolicy::Apply(*modules, kind, network);
            Promote(*modules, Capability::ServerState, serverValidated, "LevelData::mLevelName");
            if (state.displayName != lastName) {
                lastName = state.displayName;
                Diagnostics::LogLine("info", "session_world", std::format("kind={} nameLength={}",
                    Modules::SessionKindName(state.kind), state.displayName.size()));
            }
            modules->Publish(state);
        }

        if (now - lastSampleLogged >= SampleLogInterval) {
            lastSampleLogged = now;
            Diagnostics::LogLine("info", "session_telemetry_sample", std::format("connected={} ping={} kind={}",
                connected, ping == NoPing ? std::string("none") : std::to_string(ping), Modules::SessionKindName(kind)));
        }
    } catch (...) {}
}

}
