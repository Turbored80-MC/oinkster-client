#pragma once

#include "Oinkster/Events.hpp"

namespace Oinkster { class ModuleManager; }

namespace Oinkster::CombatTelemetry {

// Capability::CombatEvents, built on the game's own hurt timer
// (MobHurtTimeComponent, which Bedrock starts when the server reports that an
// entity took damage). It installs no hook: WorldTelemetry's validated
// _renderLevelPrep hook calls Observe every frame.
//
//  - LocalDamaged: the local player's hurt timer rises.
//  - LocalHitConfirmed: a local left-click arms an attack on the nearest
//    entity whose bounding box the look ray enters within reach; the hit is
//    confirmed only if that entity's hurt timer then rises within
//    AttackConfirmer::Window. A click alone is never a hit.
//  - LocalMissed is never reported: an unconfirmed attack is ambiguous
//    (invulnerability frames, a shield, lag), so it simply expires.
//
// Pending until the local player's hurt timer is first read plausibly.
void Attach(ModuleManager& modules) noexcept;
// Call only once the render thread can no longer be inside Observe.
void Detach() noexcept;

// Render thread. `eye` is the first-person camera position; `transform`
// carries the validated rotation when available.
void Observe(const void* actor, const Vec3& eye, const PlayerTransform& transform, SteadyTime now) noexcept;

}
