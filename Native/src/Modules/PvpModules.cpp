#include "Oinkster/Modules/Factories.hpp"
#include "Oinkster/Modules/HudFormat.hpp"
#include "Oinkster/Modules/PvpLogic.hpp"
#include "Oinkster/Modules/StatusLogic.hpp"
#include <algorithm>
#include <cmath>
#include <format>
#include <limits>

namespace Oinkster::Modules {

void ComboTracker::OnEvent(const CombatEvent& event) {
    switch (event.kind) {
    case CombatEventKind::LocalHitConfirmed: {
        const bool continues = lastHit_ && event.occurredAt >= *lastHit_ && event.occurredAt - *lastHit_ <= timeout_
            && event.targetId == target_ && combo_ > 0;
        combo_ = continues ? combo_ + 1 : 1;
        target_ = event.targetId;
        lastHit_ = event.occurredAt;
        best_ = std::max(best_, combo_);
        break;
    }
    case CombatEventKind::LocalMissed:
    case CombatEventKind::LocalDamaged:
        combo_ = 0;
        target_ = 0;
        lastHit_.reset();
        break;
    }
}

std::uint32_t ComboTracker::Current(SteadyTime now) const noexcept {
    if (!lastHit_ || combo_ == 0) return 0;
    if (now > *lastHit_ && now - *lastHit_ > timeout_) return 0;
    return combo_;
}

double DistanceToAabb(const Vec3& point, const Aabb& box) noexcept {
    const auto axis = [](double value, double low, double high) {
        if (low > high) std::swap(low, high);
        return value < low ? low - value : value > high ? value - high : 0.0;
    };
    const double dx = axis(point.x, box.minimum.x, box.maximum.x);
    const double dy = axis(point.y, box.minimum.y, box.maximum.y);
    const double dz = axis(point.z, box.minimum.z, box.maximum.z);
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

Vec3 LookDirection(float yawDegrees, float pitchDegrees) noexcept {
    constexpr double Radians = 3.14159265358979323846 / 180.0;
    const double yaw = static_cast<double>(yawDegrees) * Radians;
    const double pitch = static_cast<double>(pitchDegrees) * Radians;
    return {-std::sin(yaw) * std::cos(pitch), -std::sin(pitch), std::cos(yaw) * std::cos(pitch)};
}

std::optional<double> RayEntryDistance(const Vec3& origin, const Vec3& direction, const Aabb& box) noexcept {
    double entry = 0.0;
    double exit = std::numeric_limits<double>::infinity();
    const auto slab = [&](double start, double step, double low, double high) {
        if (low > high) std::swap(low, high);
        if (std::abs(step) < 1e-12) return start >= low && start <= high;
        double enter = (low - start) / step;
        double leave = (high - start) / step;
        if (enter > leave) std::swap(enter, leave);
        entry = std::max(entry, enter);
        exit = std::min(exit, leave);
        return entry <= exit;
    };
    if (!slab(origin.x, direction.x, box.minimum.x, box.maximum.x)
        || !slab(origin.y, direction.y, box.minimum.y, box.maximum.y)
        || !slab(origin.z, direction.z, box.minimum.z, box.maximum.z)) {
        return std::nullopt;
    }
    return entry;
}

std::optional<std::uint64_t> AttackConfirmer::PendingTarget(SteadyTime now) noexcept {
    if (pending_ && (now < pending_->at || now - pending_->at > Window)) pending_.reset();
    if (!pending_) return std::nullopt;
    return pending_->targetId;
}

std::optional<CombatEvent> AttackConfirmer::Observe(SteadyTime now, std::optional<int> hurtTicks) noexcept {
    if (!PendingTarget(now) || !hurtTicks) return std::nullopt;
    if (*hurtTicks <= pending_->hurtTicks) {
        // The timer only counts down; follow it so a later rise is detected.
        pending_->hurtTicks = *hurtTicks;
        return std::nullopt;
    }
    CombatEvent event{CombatEventKind::LocalHitConfirmed, now, pending_->targetId, pending_->eye, pending_->bounds};
    pending_.reset();
    return event;
}

std::vector<EquipmentWarningTracker::Warning> EquipmentWarningTracker::Evaluate(
    const EquipmentSnapshot& snapshot, int thresholdPercent, bool includeHands) {
    std::vector<Warning> warnings;
    const auto check = [&](std::size_t index, const std::optional<ItemStack>& item, std::string_view slotName) {
        auto& state = slots_[index];
        const auto percent = item ? DurabilityPercent(*item) : std::nullopt;
        const std::string itemId = item ? item->itemId : std::string{};
        if (itemId != state.itemId) state = {itemId, false};
        if (!percent) return;
        if (*percent > thresholdPercent + Hysteresis) state.warned = false;
        if (*percent <= thresholdPercent) {
            const bool isNew = !state.warned;
            state.warned = true;
            warnings.push_back({std::string(slotName), itemId, *percent, isNew});
        }
    };
    for (std::size_t index = 0; index < ArmorSlotCount; ++index) {
        check(index, snapshot.armor[index], ArmorSlotName(static_cast<ArmorSlot>(index)));
    }
    if (includeHands) {
        check(4, snapshot.mainHand, "Main hand");
        check(5, snapshot.offHand, "Off hand");
    } else {
        slots_[4] = {};
        slots_[5] = {};
    }
    return warnings;
}

std::optional<PotionKind> PotionKindOf(std::string_view itemId) noexcept {
    if (itemId == "minecraft:potion") return PotionKind::Drinkable;
    if (itemId == "minecraft:splash_potion") return PotionKind::Splash;
    if (itemId == "minecraft:lingering_potion") return PotionKind::Lingering;
    return std::nullopt;
}

PotionCounts CountPotions(const InventorySnapshot& inventory) noexcept {
    PotionCounts counts{};
    const auto limit = std::min(inventory.slots.size(), MaxInventorySlots);
    for (std::size_t index = 0; index < limit; ++index) {
        const auto& slot = inventory.slots[index];
        if (!slot || slot->count <= 0) continue;
        const auto kind = PotionKindOf(slot->itemId);
        if (!kind) continue;
        const int count = std::min(slot->count, 64);
        switch (*kind) {
        case PotionKind::Drinkable: counts.drinkable += count; break;
        case PotionKind::Splash: counts.splash += count; break;
        case PotionKind::Lingering: counts.lingering += count; break;
        }
    }
    return counts;
}

namespace {
class ComboCounterModule final : public HudModuleBase<> {
public:
    ComboCounterModule(ModuleMetadata metadata, HudLayout layout, SharedServices&)
        : HudModuleBase(std::move(metadata), {
            FloatSetting("timeout-seconds", "Combo timeout", 2.0, 0.5, 10.0, "%.1f s",
                "Resets after this long without a confirmed hit, on a miss, or when you take damage."),
            BoolSetting("show-best", "Show session best", false),
            BoolSetting("hide-when-zero", "Hide when no combo", false)}, layout) {}
    void OnSettingsChanged(const SettingsReader& settings) override {
        std::scoped_lock lock(mutex_);
        tracker_.SetTimeout(std::chrono::duration_cast<SteadyClock::duration>(
            std::chrono::duration<double>(settings.Float("timeout-seconds"))));
        showBest_ = settings.Bool("show-best");
        hideZero_ = settings.Bool("hide-when-zero");
    }
    void OnEnable() override { std::scoped_lock lock(mutex_); tracker_ = ComboTracker(); }
    void OnCombat(const CombatEvent& event) override { std::scoped_lock lock(mutex_); tracker_.OnEvent(event); }
    void OnRender(const RenderEvent& event) override { std::scoped_lock lock(mutex_); NoteRender(event); }
    [[nodiscard]] HudContent Content() const override {
        std::scoped_lock lock(mutex_);
        const auto combo = tracker_.Current(RenderNow());
        if (combo == 0 && hideZero_) return {};
        std::string text = std::format("Combo  {}", combo);
        if (showBest_) text += std::format("  best {}", tracker_.Best());
        return {{{text, combo >= 3 ? HudTone::Accent : HudTone::Normal}}, {}, {}};
    }

private:
    ComboTracker tracker_;
    bool showBest_{};
    bool hideZero_{};
};

class HitDistanceModule final : public HudModuleBase<> {
public:
    HitDistanceModule(ModuleMetadata metadata, HudLayout layout, SharedServices&)
        : HudModuleBase(std::move(metadata), {
            IntSetting("decimals", "Decimal places", 2, 1, 3),
            IntSetting("hold-seconds", "Keep last value for", 5, 1, 60, "%d s")}, layout) {}
    void OnSettingsChanged(const SettingsReader& settings) override {
        std::scoped_lock lock(mutex_);
        decimals_ = settings.Int("decimals");
        hold_ = std::chrono::seconds(settings.Int("hold-seconds"));
    }
    void OnDisable() override { std::scoped_lock lock(mutex_); last_.reset(); }
    void OnCombat(const CombatEvent& event) override {
        if (event.kind != CombatEventKind::LocalHitConfirmed) return;
        const double distance = DistanceToAabb(event.attackerEye, event.targetBounds);
        if (!std::isfinite(distance) || distance > 64.0) return;  // not a melee hit; a misread
        std::scoped_lock lock(mutex_);
        last_ = {distance, event.occurredAt};
    }
    void OnRender(const RenderEvent& event) override { std::scoped_lock lock(mutex_); NoteRender(event); }
    [[nodiscard]] HudContent Content() const override {
        std::scoped_lock lock(mutex_);
        if (!last_ || (RenderNow() > last_->second && RenderNow() - last_->second > hold_)) return {{{"Hit  --", HudTone::Muted}}, {}, {}};
        return {{{std::format("Hit  {:.{}f} m", last_->first, decimals_), HudTone::Normal}}, {}, {}};
    }

private:
    std::optional<std::pair<double, SteadyTime>> last_;
    int decimals_{2};
    SteadyClock::duration hold_{std::chrono::seconds(5)};
};

class EquipmentWarningsModule final : public HudModuleBase<> {
public:
    EquipmentWarningsModule(ModuleMetadata metadata, HudLayout layout, SharedServices&)
        : HudModuleBase(std::move(metadata), {
            IntSetting("threshold-percent", "Warn at or below", 15, 1, 90, "%d%%"),
            BoolSetting("include-hands", "Include held items", true),
            IntSetting("alert-seconds", "Highlight new warnings for", 3, 0, 15, "%d s"),
            BoolSetting("hide-when-ok", "Hide when nothing is low", true)}, layout) {}
    void OnSettingsChanged(const SettingsReader& settings) override {
        std::scoped_lock lock(mutex_);
        threshold_ = settings.Int("threshold-percent");
        includeHands_ = settings.Bool("include-hands");
        alertFor_ = std::chrono::seconds(settings.Int("alert-seconds"));
        hideWhenOk_ = settings.Bool("hide-when-ok");
    }
    void OnEnable() override { std::scoped_lock lock(mutex_); tracker_.Reset(); warnings_.clear(); lastAlert_.reset(); }
    void OnEquipment(const EquipmentSnapshot& snapshot) override {
        std::scoped_lock lock(mutex_);
        warnings_ = tracker_.Evaluate(snapshot, threshold_, includeHands_);
        if (std::ranges::any_of(warnings_, &EquipmentWarningTracker::Warning::isNew)) lastAlert_ = snapshot.capturedAt;
    }
    void OnRender(const RenderEvent& event) override { std::scoped_lock lock(mutex_); NoteRender(event); }
    [[nodiscard]] HudContent Content() const override {
        std::scoped_lock lock(mutex_);
        if (warnings_.empty()) return hideWhenOk_ ? HudContent{} : HudContent{{{"Equipment  OK", HudTone::Good}}, {}, {}};
        const bool alerting = lastAlert_ && RenderNow() >= *lastAlert_ && RenderNow() - *lastAlert_ <= alertFor_;
        HudContent content;
        for (const auto& warning : warnings_) {
            content.lines.push_back({std::format("Low {}  {}%", warning.slot, warning.percent),
                alerting ? HudTone::Warning : HudTone::Accent});
        }
        return content;
    }

private:
    EquipmentWarningTracker tracker_;
    std::vector<EquipmentWarningTracker::Warning> warnings_;
    std::optional<SteadyTime> lastAlert_;
    int threshold_{15};
    bool includeHands_{true};
    SteadyClock::duration alertFor_{std::chrono::seconds(3)};
    bool hideWhenOk_{true};
};

class PotionCounterModule final : public HudModuleBase<> {
public:
    PotionCounterModule(ModuleMetadata metadata, HudLayout layout, SharedServices&)
        : HudModuleBase(std::move(metadata), {
            BoolSetting("count-drinkable", "Count drinkable potions", true),
            BoolSetting("count-splash", "Count splash potions", true),
            BoolSetting("count-lingering", "Count lingering potions", false),
            BoolSetting("breakdown", "Show per-type breakdown", false)}, layout) {}
    void OnSettingsChanged(const SettingsReader& settings) override {
        std::scoped_lock lock(mutex_);
        drinkable_ = settings.Bool("count-drinkable");
        splash_ = settings.Bool("count-splash");
        lingering_ = settings.Bool("count-lingering");
        breakdown_ = settings.Bool("breakdown");
    }
    void OnDisable() override { std::scoped_lock lock(mutex_); counts_.reset(); }
    void OnInventory(const InventorySnapshot& snapshot) override {
        const auto counts = CountPotions(snapshot);
        std::scoped_lock lock(mutex_);
        counts_ = counts;
    }
    [[nodiscard]] HudContent Content() const override {
        std::scoped_lock lock(mutex_);
        if (!counts_) return {{{"Potions  --", HudTone::Muted}}, {}, {}};
        const int total = (drinkable_ ? counts_->drinkable : 0) + (splash_ ? counts_->splash : 0) + (lingering_ ? counts_->lingering : 0);
        HudContent content{{{std::format("Potions  {}", total), total == 0 ? HudTone::Warning : HudTone::Normal}}, {}, {}};
        if (breakdown_) {
            std::string detail;
            if (drinkable_) detail += std::format("Drink {}  ", counts_->drinkable);
            if (splash_) detail += std::format("Splash {}  ", counts_->splash);
            if (lingering_) detail += std::format("Linger {}", counts_->lingering);
            while (detail.ends_with(' ')) detail.pop_back();
            if (!detail.empty()) content.lines.push_back({detail, HudTone::Muted});
        }
        return content;
    }

private:
    std::optional<PotionCounts> counts_;
    bool drinkable_{true};
    bool splash_{true};
    bool lingering_{};
    bool breakdown_{};
};
}

std::unique_ptr<IModule> MakeComboCounter(ModuleMetadata m, HudLayout l, SharedServices& s) { return std::make_unique<ComboCounterModule>(std::move(m), l, s); }
std::unique_ptr<IModule> MakeHitDistance(ModuleMetadata m, HudLayout l, SharedServices& s) { return std::make_unique<HitDistanceModule>(std::move(m), l, s); }
std::unique_ptr<IModule> MakeEquipmentWarnings(ModuleMetadata m, HudLayout l, SharedServices& s) { return std::make_unique<EquipmentWarningsModule>(std::move(m), l, s); }
std::unique_ptr<IModule> MakePotionCounter(ModuleMetadata m, HudLayout l, SharedServices& s) { return std::make_unique<PotionCounterModule>(std::move(m), l, s); }

}
