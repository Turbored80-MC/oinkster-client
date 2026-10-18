#pragma once

#include "Oinkster/Events.hpp"
#include <span>
#include <string_view>

namespace Oinkster { class ModuleManager; }

namespace Oinkster::ServerPolicy {

struct Entry {
    ServerNetwork network{};
    std::string_view moduleId;
    bool allowed{};
    std::string_view sourceUrl;
    std::string_view reviewedOn;
};

[[nodiscard]] ServerNetwork ClassifyHostname(std::wstring_view hostname) noexcept;
[[nodiscard]] ServerNetwork ClassifyHostname(std::string_view hostname) noexcept;
[[nodiscard]] std::string_view NetworkId(ServerNetwork network) noexcept;
[[nodiscard]] std::span<const Entry> Entries() noexcept;
[[nodiscard]] const Entry* Find(ServerNetwork network, std::string_view moduleId) noexcept;
[[nodiscard]] ServerNetwork CurrentNetwork() noexcept;
[[nodiscard]] SessionKind CurrentSessionKind() noexcept;
[[nodiscard]] bool IsAllowed(SessionKind kind, ServerNetwork network, std::string_view moduleId) noexcept;

// Applies fail-closed policy through per-module capabilities. No caller can
// bypass this through a module toggle: ModuleManager removes active modules
// as soon as their policy capability becomes unavailable.
void Apply(ModuleManager& modules, SessionKind kind, ServerNetwork network);

}
