#pragma once

#include "Oinkster/Events.hpp"
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// Observational PvP feedback. Everything here consumes authoritative
// CombatEvents / snapshots from a validated adapter; nothing reacts to, or
// is derived from, local mouse clicks.
namespace Oinkster::Modules {

// Combo rule:
//   * LocalHitConfirmed on the same target within `timeout` of the previous
//     confirmed hit extends the combo by one;
//   * a confirmed hit on a different target starts a new combo at 1;
//   * the combo resets to 0 when the local player takes damage
//     (LocalDamaged), when the game reports a miss (LocalMissed), or once
//     `timeout` passes without a confirmed hit.
class ComboTracker {
public:
    explicit ComboTracker(SteadyClock::duration timeout = std::chrono::seconds(2)) : timeout_(timeout) {}
    void SetTimeout(SteadyClock::duration timeout) noexcept { timeout_ = timeout; }
    void OnEvent(const CombatEvent& event);
    [[nodiscard]] std::uint32_t Current(SteadyTime now) const noexcept;
    [[nodiscard]] std::uint32_t Best() const noexcept { return best_; }
    void Reset() noexcept { combo_ = 0; target_ = 0; lastHit_.reset(); }

private:
    SteadyClock::duration timeout_;
    std::uint32_t combo_{};
    std::uint32_t best_{};
    std::uint64_t target_{};
    std::optional<SteadyTime> lastHit_;
};

// Hit distance is measured from the attacker's eye position to the nearest
// point on the target's axis-aligned bounding box (0 when the eye is inside
// it) - the same geometry Bedrock's reach check uses, so it is directly
// comparable to the game's 3-block reach.
[[nodiscard]] double DistanceToAabb(const Vec3& point, const Aabb& box) noexcept;

// Non-spammy low-durability warnings with hysteresis. A slot warns once when
// its durability first drops to <= threshold; it re-arms only when the item
// in the slot changes (different id) or its durability recovers above
// threshold + 5 points (repair / mending).
class EquipmentWarningTracker {
public:
    struct Warning {
        std::string slot;         // "Helmet", "Main hand", ...
        std::string itemId;
        int percent{};
        bool isNew{};             // first frame this warning fired
    };
    static constexpr int Hysteresis = 5;

    std::vector<Warning> Evaluate(const EquipmentSnapshot& snapshot, int thresholdPercent, bool includeHands);
    void Reset() noexcept { slots_ = {}; }

private:
    struct SlotState { std::string itemId; bool warned{}; };
    std::array<SlotState, 6> slots_{};
};

enum class PotionKind : std::uint8_t { Drinkable, Splash, Lingering };
[[nodiscard]] std::optional<PotionKind> PotionKindOf(std::string_view itemId) noexcept;

struct PotionCounts {
    int drinkable{};
    int splash{};
    int lingering{};
    [[nodiscard]] int Total() const noexcept { return drinkable + splash + lingering; }
};
[[nodiscard]] PotionCounts CountPotions(const InventorySnapshot& inventory) noexcept;

}
