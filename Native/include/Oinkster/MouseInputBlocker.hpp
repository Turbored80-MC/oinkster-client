#pragma once

#include <cstdint>
#include <string>

namespace Oinkster { class ModuleManager; struct MouseShaping; }

namespace Oinkster::MouseInputBlocker {

enum class InstallState { Initializing, Ready, Unavailable };

[[nodiscard]] bool Install(ModuleManager& modules, std::wstring& detail) noexcept;
[[nodiscard]] InstallState PollInstall(std::wstring& detail) noexcept;
[[nodiscard]] InstallState State() noexcept;
[[nodiscard]] bool CanFilterButtons() noexcept;
void SetActive(bool active) noexcept;
// While a replay plays, the wheel and left click drive the replay hotbar
// instead of the game (the camera keeps the mouse). Wheel notches and clicks
// are queued to Dx12Overlay. Only effective when ReplayHotbarCaptured().
void SetReplayHotbar(bool enabled) noexcept;
[[nodiscard]] bool ReplayHotbarCaptured() noexcept;
// Freelook: while captured, mouse movement stops turning the player (the
// game sees a frozen position) but buttons and the wheel still reach it; the
// movement is accumulated for TakeLookDelta. Ends without a jump.
void SetLookCapture(bool capture) noexcept;
// Movement captured since the last call, in mouse counts.
void TakeLookDelta(std::int64_t& deltaX, std::int64_t& deltaY) noexcept;
// Scroll wheel disabler / sensitivity multiplier, refreshed every frame by
// Dx12Overlay. The wheel is swallowed in MouseDevice::feed; sensitivity
// scales the movement GameInput reports to the game (the source of its turn
// deltas - the same reading freelook freezes).
void SetMouseShaping(const MouseShaping& shaping) noexcept;
void Remove() noexcept;

}
