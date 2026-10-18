#pragma once

#include <string>

namespace Oinkster::ScreenTelemetry {

// Which of Minecraft's own screens is on top, for UI that belongs to one
// screen only (the main-menu Replays button). Hooks a screen-change listener
// that the game itself uses to react to the title screen ("start_screen"),
// resolved by a unique .text signature, and remembers the most recent screen
// event. Read-only; every event still reaches the game unchanged.
[[nodiscard]] bool Install(std::wstring& detail) noexcept;
void Remove() noexcept;

// False until the first screen event. Screen-specific controls must remain
// hidden until the target screen is positively identified.
[[nodiscard]] bool Known() noexcept;
[[nodiscard]] bool OnTitleScreen() noexcept;
[[nodiscard]] bool OnDisconnectedScreen() noexcept;
// True when the most recent world load the game announced was a local world
// (its own integrated server), false after a join to an external server or
// before any load was seen. Replay playback only takes over local worlds.
[[nodiscard]] bool LastLoadWasLocalWorld() noexcept;

}
