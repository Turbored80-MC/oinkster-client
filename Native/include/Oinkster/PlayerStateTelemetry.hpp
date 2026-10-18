#pragma once

#include "Oinkster/Events.hpp"

namespace Oinkster { class ModuleManager; }

namespace Oinkster::PlayerStateTelemetry {

// Local-player state read out of Bedrock's entt component registry
// (EnttRegistry) for the Actor that WorldTelemetry's validated
// InGamePlayScreen::_renderLevelPrep hook already receives every frame. It
// installs no hook of its own.
//
// Attach reports Capability::LocalPlayerRotation and Capability::Effects as
// Pending; each turns Available on its first read that resolves the
// component and passes plausibility checks, so a changed layout leaves the
// modules waiting rather than showing garbage.
void Attach(ModuleManager& modules) noexcept;
// Reports both capabilities Unavailable. Call only once the render thread can
// no longer be inside Observe (WorldTelemetry::Remove drains its hook first).
void Detach() noexcept;

// Render thread. Fills transform.yawDegrees/pitchDegrees when the rotation
// read validates, and publishes an EffectsSnapshot at most every ~200 ms.
void Observe(const void* actor, PlayerTransform& transform, SteadyTime now) noexcept;

}
