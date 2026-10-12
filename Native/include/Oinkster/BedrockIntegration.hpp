#pragma once

#include <string>

namespace Oinkster { class ModuleManager; }

namespace Oinkster::BedrockIntegration {

// Installs every reviewed, build-specific data source after the generic DX12
// and input hooks are ready. Individual capability failures are non-fatal:
// each adapter reports its own state through ModuleManager and unrelated
// adapters continue installing.
[[nodiscard]] bool Install(ModuleManager& modules, std::wstring& detail) noexcept;

// Removes installed sources in reverse order. Each component owns its own
// disable -> callback drain -> hook removal sequence.
void Remove() noexcept;

}
