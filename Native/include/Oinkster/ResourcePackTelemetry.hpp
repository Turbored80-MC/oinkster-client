#pragma once

#include "Oinkster/Events.hpp"

namespace Oinkster { class ModuleManager; }

namespace Oinkster::ResourcePackTelemetry {

// Capability::ResourcePacks. Reads the active resource-pack stacks of the
// ResourcePackManager the local Actor's ILevel returns (vtable slot 281,
// +0x8C8): the level stack (world / server packs) first, then the global
// stack (the player's own packs). No hook of its own - WorldTelemetry's
// _renderLevelPrep hook calls Observe, which re-reads every few seconds.
// Pending until the first plausible read. Names only: no version source has
// been validated.
void Attach(ModuleManager& modules) noexcept;
void Detach() noexcept;
void Observe(const void* actor, SteadyTime now) noexcept;

}
