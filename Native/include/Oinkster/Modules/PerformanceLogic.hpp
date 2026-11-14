#pragma once

#include <cstdint>
#include <string_view>

namespace Oinkster::Modules {

// FPS Boost tunes how Windows schedules the game process; it never touches
// the game itself. Values mirror the Win32 priority-class constants so this
// header stays free of <Windows.h> and testable.
inline constexpr std::uint32_t NormalPriorityClassValue = 0x00000020;      // NORMAL_PRIORITY_CLASS
inline constexpr std::uint32_t AboveNormalPriorityClassValue = 0x00008000; // ABOVE_NORMAL_PRIORITY_CLASS
inline constexpr std::uint32_t HighPriorityClassValue = 0x00000080;        // HIGH_PRIORITY_CLASS

// The "priority" choice setting. Anything unrecognised is Normal, so a
// corrupted setting can never raise the process above what the user chose.
[[nodiscard]] constexpr std::uint32_t BoostPriorityClass(std::string_view choice) noexcept {
    if (choice == "above-normal") return AboveNormalPriorityClassValue;
    if (choice == "high") return HighPriorityClassValue;
    return NormalPriorityClassValue;
}

struct ProcessBoostConfig {
    std::uint32_t priorityClass{AboveNormalPriorityClassValue};
    bool disablePowerThrottling{true};
    bool highResolutionTimer{true};
    friend bool operator==(const ProcessBoostConfig&, const ProcessBoostConfig&) = default;
};

}
