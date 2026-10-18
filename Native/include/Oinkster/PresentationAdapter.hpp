#pragma once

#include "Oinkster/Events.hpp"
#include <optional>
#include <string>

namespace Oinkster { class ModuleManager; }

namespace Oinkster::PresentationAdapter {

// Applies the enabled presentation modules' PresentationProfile to the local
// render path. Unlike the telemetry adapters this one changes values the game
// renders with - always local, never packets, world state or physics:
//  - CameraPresentation (FOV profiles, zoom, view bobbing): the active
//    camera's CameraComponent FOV is overridden only for the duration of the
//    LevelRendererPlayer camera update that builds the projection from it,
//    then restored; the Options view-bobbing getter is answered false while
//    bobbing strength is 0.
//  - CameraRotation / CameraMotion / ThirdPersonPresentation (freelook,
//    cinematic camera, hurt camera, third-person distance): the same camera's
//    rotation quaternion and position, overridden for that update only.
//  - LightingPresentation (fullbright): the Options gamma getter.
//  - TimePresentation (time changer): the sky's time-of-day function, on the
//    render thread only, so a local world's integrated server keeps real time.
// Every hook target is a unique .text signature; each capability stays
// Pending until its hook has seen a plausible value in the running game.
[[nodiscard]] bool Install(ModuleManager& modules, std::wstring& detail) noexcept;
void Remove() noexcept;
// The local player, from WorldTelemetry's _renderLevelPrep hook: the pivot
// third-person cameras orbit.
void ObserveLocalPlayer(const void* actor) noexcept;
// Snap look: the perspective the game is being told to render from right
// now, if a module overrides it (CameraTelemetry reports this instead of
// the stored option so the crosshair and camera tools follow the snap).
[[nodiscard]] std::optional<CameraPerspective> PerspectiveOverride() noexcept;

}
