#pragma once

#include "Oinkster/Events.hpp"
#include "Oinkster/Module.hpp"
#include "Oinkster/Modules/RollingWindow.hpp"
#include <array>
#include <optional>
#include <string>
#include <vector>

namespace Oinkster::Modules {

// Input latency, defined precisely:
//
//   the time from the Minecraft window procedure receiving an input message
//   (key down, mouse button down, or wheel) to the start of the next Present
//   call Oinkster observes for that window.
//
// It measures input-to-next-rendered-frame inside the process. It does NOT
// include USB/OS input latency, the GPU's time to finish the frame, or
// display scan-out - the HUD labels it "Input to frame" for that reason.
// When several inputs land between two frames, the earliest one is used
// (that is the one that waited longest).
class InputLatencyMeter {
public:
    static constexpr double MaxPlausibleMilliseconds = 1000.0;
    explicit InputLatencyMeter(std::size_t capacity = 120) : samples_(capacity) {}

    void OnInput(SteadyTime at);
    void OnFrame(SteadyTime presentedAt);
    void Clear();

    [[nodiscard]] std::optional<double> LastMilliseconds() const;
    [[nodiscard]] std::optional<double> AverageMilliseconds() const;
    [[nodiscard]] std::optional<double> MaximumMilliseconds() const;
    [[nodiscard]] std::size_t SampleCount() const noexcept { return samples_.Size(); }

private:
    RingBuffer<double> samples_;
    std::optional<SteadyTime> pending_;
};

// Pure, deterministic click filters used by the runtime modules and native
// tests. Both operate only on button-down events and never queue or replay an
// input. A backwards clock step clears that button's history and allows the
// current click so stale timestamps cannot lock input out.
class ClickDebouncer {
public:
    void Configure(std::chrono::milliseconds left, std::chrono::milliseconds right, bool linked) noexcept;
    [[nodiscard]] bool Allow(std::uint32_t button, SteadyTime at) noexcept;
    void Clear() noexcept;

private:
    std::chrono::milliseconds left_{10};
    std::chrono::milliseconds right_{10};
    std::optional<SteadyTime> lastLeft_;
    std::optional<SteadyTime> lastRight_;
};

class CpsLimiter {
public:
    void Configure(std::size_t left, std::size_t right) noexcept;
    [[nodiscard]] bool Allow(std::uint32_t button, SteadyTime at) noexcept;
    void Clear() noexcept;

private:
    EventRateWindow left_{64};
    EventRateWindow right_{64};
    std::optional<SteadyTime> lastLeft_;
    std::optional<SteadyTime> lastRight_;
    std::size_t leftLimit_{15};
    std::size_t rightLimit_{15};
};

// Null Movement (snap tap): of two opposing movement keys only the newest
// press reaches the game. Releasing it hands the axis back to the older key
// if that is still physically held. Never sends a key the player is not
// holding, and never more than one key per axis.
class NullMovementFilter {
public:
    struct Axis { std::uint32_t first{}; std::uint32_t second{}; };
    void Configure(std::array<Axis, 2> axes) noexcept;
    // Returns false when `key` is not on a configured axis (the message goes
    // through unchanged); otherwise `out` holds the messages to send instead.
    [[nodiscard]] bool Transform(std::uint32_t key, bool pressed, bool repeated,
        const std::array<bool, 256>& physical, std::vector<KeyMessage>& out);
    // The game's view is unknown (screen, menu, focus loss): start over.
    void Reset() noexcept;

private:
    std::array<Axis, 2> axes_{{{'A', 'D'}, {'W', 'S'}}};
    std::array<bool, 256> sent_{};
};

// Display label for a Win32 virtual key ("W", "Space", "Shift", "LMB" ...).
[[nodiscard]] std::string ShortKeyLabel(std::uint32_t key);

}
