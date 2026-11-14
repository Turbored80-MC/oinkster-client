#include "Oinkster/ServerPolicy.hpp"
#include "Oinkster/Capabilities.hpp"
#include "Oinkster/ModuleManager.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cwctype>
#include <string>

namespace Oinkster::ServerPolicy {
namespace {
constexpr std::string_view ReviewedOn = "2026-10-16";
constexpr std::array<Entry, 14> Policies{{
    {ServerNetwork::Zeqa, "dc-prevent", true, "https://github.com/Zeqa-Network/DCPrevent", ReviewedOn},
    {ServerNetwork::Zeqa, "cps-cap", false, "https://github.com/Zeqa-Network/DCPrevent", ReviewedOn},
    {ServerNetwork::Hive, "dc-prevent", false, "https://support.playhive.com/allowed-and-denied-mods/", ReviewedOn},
    {ServerNetwork::Hive, "cps-cap", false, "https://support.playhive.com/allowed-and-denied-mods/", ReviewedOn},
    {ServerNetwork::CubeCraft, "dc-prevent", false, "https://www.cubecraft.net/threads/allowed-mods-and-clients.228596/", ReviewedOn},
    {ServerNetwork::CubeCraft, "cps-cap", false, "https://www.cubecraft.net/threads/allowed-mods-and-clients.228596/", ReviewedOn},
    {ServerNetwork::Galaxite, "dc-prevent", false, "https://www.galaxite.net/rules/", ReviewedOn},
    {ServerNetwork::Galaxite, "cps-cap", false, "https://www.galaxite.net/rules/", ReviewedOn},
    {ServerNetwork::Lifeboat, "dc-prevent", false, "https://www.lbsg.net/in-game-rules/", ReviewedOn},
    {ServerNetwork::Lifeboat, "cps-cap", false, "https://www.lbsg.net/in-game-rules/", ReviewedOn},
    {ServerNetwork::NetherGames, "dc-prevent", false, "https://support.nethergames.org/terms-of-service", ReviewedOn},
    {ServerNetwork::NetherGames, "cps-cap", false, "https://support.nethergames.org/terms-of-service", ReviewedOn},
    {ServerNetwork::Mineville, "dc-prevent", false, "", ReviewedOn},
    {ServerNetwork::Mineville, "cps-cap", false, "", ReviewedOn},
}};
std::atomic_uint8_t currentNetwork{static_cast<std::uint8_t>(ServerNetwork::Local)};
std::atomic_uint8_t currentKind{static_cast<std::uint8_t>(SessionKind::None)};

std::wstring Normalize(std::wstring_view hostname) {
    std::wstring value(hostname);
    while (!value.empty() && value.back() == L'.') value.pop_back();
    std::ranges::transform(value, value.begin(), [](wchar_t character) {
        return static_cast<wchar_t>(std::towlower(character));
    });
    return value;
}

bool IsDomain(std::wstring_view host, std::wstring_view domain) noexcept {
    return host == domain || (host.size() > domain.size()
        && host.ends_with(domain) && host[host.size() - domain.size() - 1] == L'.');
}

Capability CapabilityFor(std::string_view moduleId) noexcept {
    return moduleId == "dc-prevent" ? Capability::DcPreventPolicy : Capability::CpsCapPolicy;
}

void Set(ModuleManager& modules, std::string_view moduleId, bool allowed, std::wstring reason) {
    modules.SetCapability(CapabilityFor(moduleId), allowed
        ? CapabilityStatus{CapabilityState::Available, {}}
        : CapabilityStatus{CapabilityState::Unavailable, std::move(reason)});
}
}

ServerNetwork ClassifyHostname(std::wstring_view hostname) noexcept {
    try {
        const auto host = Normalize(hostname);
        if (IsDomain(host, L"zeqa.net")) return ServerNetwork::Zeqa;
        if (IsDomain(host, L"hivebedrock.network") || IsDomain(host, L"playhive.com")) return ServerNetwork::Hive;
        if (IsDomain(host, L"cubecraft.net")) return ServerNetwork::CubeCraft;
        if (IsDomain(host, L"galaxite.net")) return ServerNetwork::Galaxite;
        if (IsDomain(host, L"lbsg.net")) return ServerNetwork::Lifeboat;
        if (IsDomain(host, L"nethergames.org") || IsDomain(host, L"ngmc.co")) return ServerNetwork::NetherGames;
        if (IsDomain(host, L"inpvp.net") || IsDomain(host, L"mineville.org")) return ServerNetwork::Mineville;
    } catch (...) {}
    return ServerNetwork::UnknownRemote;
}

ServerNetwork ClassifyHostname(std::string_view hostname) noexcept {
    try { return ClassifyHostname(std::wstring(hostname.begin(), hostname.end())); }
    catch (...) { return ServerNetwork::UnknownRemote; }
}

std::string_view NetworkId(ServerNetwork network) noexcept {
    switch (network) {
    case ServerNetwork::Local: return "local";
    case ServerNetwork::Zeqa: return "zeqa";
    case ServerNetwork::Hive: return "hive";
    case ServerNetwork::CubeCraft: return "cubecraft";
    case ServerNetwork::Galaxite: return "galaxite";
    case ServerNetwork::Lifeboat: return "lifeboat";
    case ServerNetwork::NetherGames: return "nethergames";
    case ServerNetwork::Mineville: return "mineville";
    case ServerNetwork::UnknownRemote: return "unknown-remote";
    }
    return "unknown-remote";
}

std::span<const Entry> Entries() noexcept { return Policies; }

const Entry* Find(ServerNetwork network, std::string_view moduleId) noexcept {
    const auto found = std::ranges::find_if(Policies, [&](const Entry& entry) {
        return entry.network == network && entry.moduleId == moduleId;
    });
    return found == Policies.end() ? nullptr : &*found;
}

ServerNetwork CurrentNetwork() noexcept {
    return static_cast<ServerNetwork>(currentNetwork.load(std::memory_order_acquire));
}

SessionKind CurrentSessionKind() noexcept {
    return static_cast<SessionKind>(currentKind.load(std::memory_order_acquire));
}

bool IsAllowed(SessionKind kind, ServerNetwork network, std::string_view moduleId) noexcept {
    if (kind == SessionKind::Singleplayer || kind == SessionKind::Lan) return true;
    if (kind == SessionKind::None) return false;
    if (const auto* policy = Find(network, moduleId)) return policy->allowed;
    return false;
}

void Apply(ModuleManager& modules, SessionKind kind, ServerNetwork network) {
    currentKind.store(static_cast<std::uint8_t>(kind), std::memory_order_release);
    currentNetwork.store(static_cast<std::uint8_t>(network), std::memory_order_release);
    if (IsAllowed(kind, network, "dc-prevent") && IsAllowed(kind, network, "cps-cap")) {
        Set(modules, "dc-prevent", true, {});
        Set(modules, "cps-cap", true, {});
        return;
    }

    for (const auto moduleId : {std::string_view("dc-prevent"), std::string_view("cps-cap")}) {
        if (IsAllowed(kind, network, moduleId)) {
            Set(modules, moduleId, true, {});
            continue;
        }
        std::wstring reason = L"Blocked by the fail-closed competitive policy for ";
        const auto networkId = NetworkId(network);
        reason.append(networkId.begin(), networkId.end());
        reason += L". No explicit permission is recorded for this module (rules reviewed 2026-10-16).";
        Set(modules, moduleId, false, std::move(reason));
    }
}

}
