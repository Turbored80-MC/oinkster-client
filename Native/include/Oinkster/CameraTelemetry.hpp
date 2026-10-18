#pragma once

#include "Oinkster/Events.hpp"

namespace Oinkster { class ModuleManager; }

namespace Oinkster::CameraTelemetry {

// Publishes CameraPerspectiveEvent from the game's own perspective option
// (Options::getPlayerViewPerspective: 0 first person, 1 third person back,
// 2 third person front), reached from the local player's ClientInstance by
// plain field reads - no game function is called. No hook of its own:
// WorldTelemetry's _renderLevelPrep hook calls Observe. Every step is
// validated against the build's code before it is trusted; any mismatch
// publishes Unknown. Publishes Unknown on Detach.
void Attach(ModuleManager& modules) noexcept;
void Detach() noexcept;
void Observe(const void* actor, SteadyTime now) noexcept;
// The last perspective published (Unknown when none is validated).
[[nodiscard]] CameraPerspective Current() noexcept;

}
