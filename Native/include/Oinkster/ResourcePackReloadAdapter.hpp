#pragma once

#include <span>
#include <string>

namespace Oinkster { class ModuleManager; }

namespace Oinkster::ResourcePackReloadAdapter {

// Installs only after an exact-build signature, owner object, and game-thread
// call context all validate. The current build intentionally fails closed.
[[nodiscard]] bool Install(ModuleManager& modules, std::wstring& detail) noexcept;
[[nodiscard]] bool Reload(std::span<const std::string> orderedUuids, std::wstring& error) noexcept;
[[nodiscard]] bool Available() noexcept;
void Remove() noexcept;

}
