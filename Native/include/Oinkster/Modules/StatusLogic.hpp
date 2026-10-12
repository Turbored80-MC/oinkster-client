#pragma once

#include "Oinkster/Events.hpp"
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace Oinkster::Modules {

// Inventory snapshots are bounded by consumers: slots past this are ignored
// whatever length an adapter reports.
inline constexpr std::size_t MaxInventorySlots = 64;

// Explicit-time stopwatch. Transitions:
//   Reset   --Start/Toggle-->  Running
//   Running --Pause/Toggle-->  Paused
//   Paused  --Resume/Start/Toggle--> Running
//   any     --Reset-->         Reset (elapsed 0)
// Calls that do not apply to the current state are no-ops.
class Stopwatch {
public:
    enum class State : std::uint8_t { Reset, Running, Paused };

    void Start(SteadyTime now);
    void Pause(SteadyTime now);
    void Resume(SteadyTime now);
    void Toggle(SteadyTime now);
    void Reset() noexcept;
    [[nodiscard]] State Current() const noexcept { return state_; }
    [[nodiscard]] std::chrono::milliseconds Elapsed(SteadyTime now) const;

private:
    State state_{State::Reset};
    SteadyClock::duration accumulated_{};
    SteadyTime runningSince_{};
};

// Local session counters. Reset policy: everything zeroes when the module is
// enabled and when the player presses Reset; nothing is persisted.
struct SessionCounters {
    SteadyTime startedAt{};
    std::uint64_t keyPresses{};
    std::uint64_t leftClicks{};
    std::uint64_t rightClicks{};
    double distanceBlocks{};
    std::optional<Vec3> lastPosition;
    std::optional<SteadyTime> lastPositionAt;

    void Reset(SteadyTime now);
    // Adds the step from the previous position, skipping teleports and gaps
    // with the same thresholds as SpeedEstimator.
    void AddPosition(const PlayerTransform& transform);
};

// Remaining durability in percent [0, 100], or nullopt for items without
// validated durability (max <= 0, or remaining outside [0, max]).
[[nodiscard]] std::optional<int> DurabilityPercent(const ItemStack& item) noexcept;

// Total count of `itemId` across the (bounded) inventory.
[[nodiscard]] int CountItem(const InventorySnapshot& inventory, std::string_view itemId) noexcept;
[[nodiscard]] const ItemStack* SelectedItem(const InventorySnapshot& inventory) noexcept;

// True for text that looks like a network address (IPv4, IPv6, host:port or
// a bare hostname). Server display masks these unless the player opts in.
[[nodiscard]] bool LooksLikeAddress(std::wstring_view value) noexcept;

// "Speed II  1:24", "Night vision  **:**" (infinite, as Bedrock shows it).
[[nodiscard]] std::string FormatStatusEffect(const StatusEffect& effect, bool showLevel);

[[nodiscard]] std::string_view ArmorSlotName(ArmorSlot slot) noexcept;
[[nodiscard]] std::string_view SessionKindName(SessionKind kind) noexcept;

}
