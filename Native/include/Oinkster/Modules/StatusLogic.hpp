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

// One MobEffectInstance as read from game memory: the effect's numeric id,
// remaining duration in game ticks (-1 = infinite) and 0-based amplifier.
struct MobEffectRecord {
    std::uint32_t id{};
    std::int32_t durationTicks{};
    std::int32_t amplifier{};
};

// Bedrock's numeric effect id -> "minecraft:<name>", or an empty view for ids
// this build's table does not name.
[[nodiscard]] std::string_view EffectIdName(std::uint32_t id) noexcept;
// The typed effect for a record, or nullopt when the slot is empty, expired
// or implausible (unknown ids still pass, named "minecraft:effect_<id>").
[[nodiscard]] std::optional<StatusEffect> ToStatusEffect(const MobEffectRecord& record);

// One ItemStack as read from game memory. Max durability is not read from the
// game (that needs a call into Item::getComponent); it comes from
// VanillaMaxDurability, so custom items report no durability.
struct ItemRecord {
    std::string itemNamespace;   // empty = "minecraft"
    std::string name;            // raw name id, e.g. "diamond_sword"
    int count{};
    std::optional<int> damage;   // the "Damage" NBT tag; nullopt = untouched
};

// Lowercase identifier text as Bedrock uses it for namespaces and item names.
[[nodiscard]] bool IsPlausibleIdentifier(std::string_view text) noexcept;
// Vanilla max durability by namespaced id ("minecraft:iron_boots" -> 195).
[[nodiscard]] std::optional<int> VanillaMaxDurability(std::string_view itemId) noexcept;
// The typed stack for a record, or nullopt for empty or implausible slots.
[[nodiscard]] std::optional<ItemStack> ToItemStack(const ItemRecord& record);

[[nodiscard]] std::string_view ArmorSlotName(ArmorSlot slot) noexcept;
[[nodiscard]] std::string_view SessionKindName(SessionKind kind) noexcept;

}
