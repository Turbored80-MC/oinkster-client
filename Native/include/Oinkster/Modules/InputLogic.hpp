#pragma once

#include "Oinkster/Events.hpp"
#include "Oinkster/Modules/RollingWindow.hpp"
#include <optional>
#include <string>

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

// Display label for a Win32 virtual key ("W", "Space", "Shift", "LMB" ...).
[[nodiscard]] std::string ShortKeyLabel(std::uint32_t key);

}
