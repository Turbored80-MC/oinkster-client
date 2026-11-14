#pragma once

#include "Oinkster/Events.hpp"
#include <optional>
#include <string>

namespace Oinkster { class ModuleManager; }

namespace Oinkster::WorldTelemetry {

// Resolves "ScreenView::setupAndRender" via a .text byte-signature scan (the
// pattern Flarial dll-oss maintains for its 1.26.X branch) and hooks it
// through HookManager to read the local camera position each frame.
//
// Fail-closed: the hook installs only when exactly one location across all
// candidate patterns matches (SignatureScan::FindUniqueInText). On success
// it reports Capability::LocalPlayerPosition Available and publishes a
// validated PlayerTransform (finite, inside the world border) at most every
// ~16 ms; on any failure the capability is reported Unavailable with the
// specific reason and nothing is hooked.
//
// Validation status: the signature and offset chain are taken from a
// maintained third-party table, not re-derived here. Live validation
// against Minecraft 1.26.5203 is still required; until it has been done,
// treat the "coordinates" output as camera position, which differs from the
// player's feet position (and in third-person view, from the player).
[[nodiscard]] bool Install(ModuleManager& modules, std::wstring& detail) noexcept;
void Remove() noexcept;

// True while the hook is installed.
[[nodiscard]] bool Attached() noexcept;
// True when Minecraft prepared a level for rendering within `window` of
// `now`, i.e. the player is in a world (the main menu renders no level).
[[nodiscard]] bool LevelRenderedWithin(SteadyTime now, SteadyClock::duration window) noexcept;

// The local camera as last read: eye position and, when the rotation
// component validated, yaw/pitch (Bedrock convention, see PlayerTransform).
struct View {
    SteadyTime capturedAt{};
    Vec3 eye{};
    std::optional<float> yawDegrees;
    std::optional<float> pitchDegrees;
};
[[nodiscard]] std::optional<View> LatestView() noexcept;

}
