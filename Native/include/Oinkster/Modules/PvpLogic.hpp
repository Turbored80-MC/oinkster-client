#pragma once

#include "Oinkster/Events.hpp"
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// Observational PvP feedback. Modules consume authoritative CombatEvents /
// snapshots from a validated adapter; a hit is never derived from a local
// click alone (see AttackConfirmer).
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

// Unit view vector for a Bedrock rotation (yaw 0 = +Z, 90 = -X; pitch
// positive looks down).
[[nodiscard]] Vec3 LookDirection(float yawDegrees, float pitchDegrees) noexcept;

// Distance along a unit ray to where it enters `box` (0 when the origin is
// inside), or nullopt when the ray misses it.
[[nodiscard]] std::optional<double> RayEntryDistance(const Vec3& origin, const Vec3& direction, const Aabb& box) noexcept;

// Turns the game's own hurt timer into confirmed hits (used by the combat
// adapter). An attack is only *armed* by a local click on the entity under the
// look ray; it is confirmed only when that entity's hurt timer - which
// Bedrock starts when the server reports the entity was damaged - rises
// within `window`. A click alone never yields a hit, and an armed attack that
// expires produces nothing (invulnerability frames make a "miss" ambiguous).
class AttackConfirmer {
public:
    struct Attack {
        SteadyTime at{};
        std::uint64_t targetId{};
        Vec3 eye{};
        Aabb bounds{};
        int hurtTicks{};  // the target's hurt timer when the attack was armed
    };
    static constexpr auto Window = std::chrono::milliseconds(800);

    void Arm(const Attack& attack) noexcept { pending_ = attack; }
    [[nodiscard]] std::optional<std::uint64_t> PendingTarget(SteadyTime now) noexcept;
    // `hurtTicks` is the pending target's current hurt timer (nullopt when it
    // could not be read this frame).
    [[nodiscard]] std::optional<CombatEvent> Observe(SteadyTime now, std::optional<int> hurtTicks) noexcept;
    void Clear() noexcept { pending_.reset(); }

private:
    std::optional<Attack> pending_;
};

// LocalDamaged whenever the local player's hurt timer rises.
class HurtRiseWatcher {
public:
    [[nodiscard]] bool Observe(int hurtTicks) noexcept {
        const bool rose = last_ && hurtTicks > *last_;
        last_ = hurtTicks;
        return rose;
    }
    void Clear() noexcept { last_.reset(); }

private:
    std::optional<int> last_;
};

// Bedrock's hurt timer starts at 10 ticks; anything outside this is a misread.
[[nodiscard]] constexpr bool IsPlausibleHurtTicks(int ticks) noexcept { return ticks >= 0 && ticks <= 20; }

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
