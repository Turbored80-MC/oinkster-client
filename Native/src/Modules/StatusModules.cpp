#include "Oinkster/Modules/Factories.hpp"
#include "Oinkster/Modules/HudFormat.hpp"
#include "Oinkster/Modules/StatusLogic.hpp"
#include "Oinkster/Modules/TelemetryLogic.hpp"
#include <algorithm>
#include <cmath>
#include <ctime>
#include <cwctype>
#include <format>

namespace Oinkster::Modules {

void Stopwatch::Start(SteadyTime now) {
    if (state_ == State::Running) return;
    runningSince_ = now;
    state_ = State::Running;
}

void Stopwatch::Pause(SteadyTime now) {
    if (state_ != State::Running) return;
    if (now > runningSince_) accumulated_ += now - runningSince_;
    state_ = State::Paused;
}

void Stopwatch::Resume(SteadyTime now) {
    if (state_ == State::Paused) Start(now);
}

void Stopwatch::Toggle(SteadyTime now) {
    if (state_ == State::Running) Pause(now); else Start(now);
}

void Stopwatch::Reset() noexcept {
    state_ = State::Reset;
    accumulated_ = {};
    runningSince_ = {};
}

std::chrono::milliseconds Stopwatch::Elapsed(SteadyTime now) const {
    auto total = accumulated_;
    if (state_ == State::Running && now > runningSince_) total += now - runningSince_;
    return std::chrono::duration_cast<std::chrono::milliseconds>(total);
}

void SessionCounters::Reset(SteadyTime now) {
    *this = SessionCounters{};
    startedAt = now;
}

void SessionCounters::AddPosition(const PlayerTransform& transform) {
    if (!IsPlausibleWorldPosition(transform.position)) return;
    if (lastPosition && lastPositionAt && transform.capturedAt > *lastPositionAt) {
        const double dx = transform.position.x - lastPosition->x;
        const double dy = transform.position.y - lastPosition->y;
        const double dz = transform.position.z - lastPosition->z;
        const double step = std::sqrt(dx * dx + dy * dy + dz * dz);
        const auto gap = transform.capturedAt - *lastPositionAt;
        const double seconds = std::chrono::duration<double>(gap).count();
        const SpeedEstimator::Config limits;
        if (gap <= limits.maxGap && step / seconds <= limits.maxPlausibleSpeed) distanceBlocks += step;
    }
    lastPosition = transform.position;
    lastPositionAt = transform.capturedAt;
}

std::optional<int> DurabilityPercent(const ItemStack& item) noexcept {
    if (!item.durabilityMax || !item.durabilityRemaining) return std::nullopt;
    const int maximum = *item.durabilityMax;
    const int remaining = *item.durabilityRemaining;
    if (maximum <= 0 || remaining < 0 || remaining > maximum) return std::nullopt;
    return static_cast<int>((static_cast<long long>(remaining) * 100 + maximum / 2) / maximum);
}

int CountItem(const InventorySnapshot& inventory, std::string_view itemId) noexcept {
    if (itemId.empty()) return 0;
    long long total{};
    const auto limit = std::min(inventory.slots.size(), MaxInventorySlots);
    for (std::size_t index = 0; index < limit; ++index) {
        const auto& slot = inventory.slots[index];
        if (slot && slot->itemId == itemId && slot->count > 0) total += std::min(slot->count, 255);
    }
    return static_cast<int>(std::min<long long>(total, 1'000'000));
}

const ItemStack* SelectedItem(const InventorySnapshot& inventory) noexcept {
    // The selected slot is a hotbar index (0-8) into the first nine slots.
    if (inventory.selectedHotbarSlot < 0 || inventory.selectedHotbarSlot > 8) return nullptr;
    const auto index = static_cast<std::size_t>(inventory.selectedHotbarSlot);
    if (index >= inventory.slots.size() || !inventory.slots[index] || inventory.slots[index]->count <= 0) return nullptr;
    return &*inventory.slots[index];
}

bool LooksLikeAddress(std::wstring_view value) noexcept {
    while (!value.empty() && std::iswspace(value.front())) value.remove_prefix(1);
    while (!value.empty() && std::iswspace(value.back())) value.remove_suffix(1);
    if (value.empty()) return false;
    // Anything with whitespace reads as a human-chosen name ("My World").
    if (std::ranges::any_of(value, [](wchar_t ch) { return std::iswspace(ch) != 0; })) return false;
    const auto colons = std::ranges::count(value, L':');
    if (colons >= 2) return true;  // IPv6
    const auto host = colons == 1 ? value.substr(0, value.find(L':')) : value;
    if (colons == 1) {
        const auto port = value.substr(value.find(L':') + 1);
        if (!port.empty() && std::ranges::all_of(port, [](wchar_t ch) { return ch >= L'0' && ch <= L'9'; })) return true;
    }
    // Dotted host or IPv4: only [A-Za-z0-9.-] with at least one interior dot.
    const bool hostChars = std::ranges::all_of(host, [](wchar_t ch) {
        return (ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z') || (ch >= L'0' && ch <= L'9') || ch == L'.' || ch == L'-';
    });
    const auto dot = host.find(L'.');
    return hostChars && dot != std::wstring_view::npos && dot > 0 && dot + 1 < host.size();
}

std::string FormatStatusEffect(const StatusEffect& effect, bool showLevel) {
    std::string name = effect.displayName.empty() ? ReadableItemId(effect.effectId) : ToUtf8(effect.displayName);
    if (!name.empty()) name[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(name[0])));
    if (showLevel && effect.amplifier > 0) name += " " + EffectLevel(effect.amplifier);
    const std::string remaining = effect.remaining ? FormatDuration(*effect.remaining) : std::string("**:**");
    return name + "  " + remaining;
}

std::string_view ArmorSlotName(ArmorSlot slot) noexcept {
    switch (slot) {
    case ArmorSlot::Head: return "Helmet";
    case ArmorSlot::Chest: return "Chestplate";
    case ArmorSlot::Legs: return "Leggings";
    case ArmorSlot::Feet: return "Boots";
    }
    return "Armor";
}

std::string_view SessionKindName(SessionKind kind) noexcept {
    switch (kind) {
    case SessionKind::None: return "Menu";
    case SessionKind::Singleplayer: return "Singleplayer";
    case SessionKind::Lan: return "LAN";
    case SessionKind::Realm: return "Realm";
    case SessionKind::Server: return "Server";
    }
    return "Menu";
}

namespace {
std::string ItemName(const ItemStack& item) {
    std::string name = item.displayName.empty() ? ReadableItemId(item.itemId) : ToUtf8(item.displayName);
    return Ellipsize(name, 28);
}

std::string DurabilityText(const ItemStack& item, const std::string& mode) {
    const auto percent = DurabilityPercent(item);
    if (!percent) return "--";
    if (mode == "remaining") return std::format("{}/{}", *item.durabilityRemaining, *item.durabilityMax);
    if (mode == "both") return std::format("{}/{}  {}%", *item.durabilityRemaining, *item.durabilityMax, *percent);
    return std::format("{}%", *percent);
}

HudTone DurabilityTone(const ItemStack& item, int warnPercent) {
    const auto percent = DurabilityPercent(item);
    return percent && *percent <= warnPercent ? HudTone::Warning : HudTone::Normal;
}

std::vector<SettingChoice> DurabilityModes() {
    return {{"percent", "Percent"}, {"remaining", "Remaining / max"}, {"both", "Both"}};
}

class ClockModule final : public HudModuleBase<> {
public:
    ClockModule(ModuleMetadata metadata, HudLayout layout, SharedServices& services)
        : HudModuleBase(std::move(metadata), {
            BoolSetting("24-hour", "24-hour time", false),
            BoolSetting("show-seconds", "Show seconds", false),
            BoolSetting("show-label", "Show label", true)}, layout), wallClock_(services.wallClock) {}
    void OnSettingsChanged(const SettingsReader& settings) override {
        std::scoped_lock lock(mutex_);
        use24_ = settings.Bool("24-hour");
        showSeconds_ = settings.Bool("show-seconds");
        showLabel_ = settings.Bool("show-label");
    }
    [[nodiscard]] HudContent Content() const override {
        bool use24{}, seconds{}, label{};
        {
            std::scoped_lock lock(mutex_);
            use24 = use24_; seconds = showSeconds_; label = showLabel_;
        }
        const std::time_t now = std::chrono::system_clock::to_time_t(wallClock_());
        std::tm local{};
        if (localtime_s(&local, &now) != 0) return {{{"Time  --", HudTone::Muted}}, {}, {}};
        const auto text = FormatClock(local.tm_hour, local.tm_min, local.tm_sec, use24, seconds);
        return {{{label ? "Time  " + text : text, HudTone::Normal}}, {}, {}};
    }

private:
    std::function<std::chrono::system_clock::time_point()> wallClock_;
    bool use24_{};
    bool showSeconds_{};
    bool showLabel_{true};
};

class StopwatchModule final : public HudModuleBase<> {
public:
    StopwatchModule(ModuleMetadata metadata, HudLayout layout, SharedServices&)
        : HudModuleBase(std::move(metadata), {
            BoolSetting("show-milliseconds", "Show milliseconds", false),
            BoolSetting("start-on-enable", "Start automatically when enabled", false),
            KeySetting("toggle-key", "Start / pause key", 0, "Optional. Must not be a movement or module key you rely on.")}, layout) {}
    void OnSettingsChanged(const SettingsReader& settings) override {
        std::scoped_lock lock(mutex_);
        showMilliseconds_ = settings.Bool("show-milliseconds");
        startOnEnable_ = settings.Bool("start-on-enable");
        toggleKey_ = settings.Key("toggle-key");
    }
    void OnEnable() override {
        std::scoped_lock lock(mutex_);
        stopwatch_.Reset();
        if (startOnEnable_) stopwatch_.Start(SteadyClock::now());
    }
    void OnKeyboard(const KeyboardEvent& event) override {
        if (event.uiCaptured || !event.pressed || event.repeated) return;
        std::scoped_lock lock(mutex_);
        if (toggleKey_ != 0 && event.key == toggleKey_) stopwatch_.Toggle(event.occurredAt);
    }
    void OnRender(const RenderEvent& event) override { std::scoped_lock lock(mutex_); NoteRender(event); }
    [[nodiscard]] std::vector<ModuleAction> Actions() const override {
        std::scoped_lock lock(mutex_);
        switch (stopwatch_.Current()) {
        case Stopwatch::State::Reset: return {{"start", "Start"}};
        case Stopwatch::State::Running: return {{"pause", "Pause"}, {"reset", "Reset"}};
        case Stopwatch::State::Paused: return {{"resume", "Resume"}, {"reset", "Reset"}};
        }
        return {};
    }
    bool InvokeAction(std::string_view action, SteadyTime now) override {
        std::scoped_lock lock(mutex_);
        if (action == "start") stopwatch_.Start(now);
        else if (action == "pause") stopwatch_.Pause(now);
        else if (action == "resume") stopwatch_.Resume(now);
        else if (action == "reset") stopwatch_.Reset();
        else return false;
        return true;
    }
    [[nodiscard]] HudContent Content() const override {
        std::scoped_lock lock(mutex_);
        const auto text = "Stopwatch  " + FormatDuration(stopwatch_.Elapsed(RenderNow()), showMilliseconds_);
        const auto tone = stopwatch_.Current() == Stopwatch::State::Running ? HudTone::Normal
            : stopwatch_.Current() == Stopwatch::State::Paused ? HudTone::Accent : HudTone::Muted;
        return {{{text, tone}}, {}, {}};
    }

private:
    Stopwatch stopwatch_;
    bool showMilliseconds_{};
    bool startOnEnable_{};
    std::uint32_t toggleKey_{};
};

class SessionStatisticsModule final : public HudModuleBase<> {
public:
    SessionStatisticsModule(ModuleMetadata metadata, HudLayout layout, SharedServices&)
        : HudModuleBase(std::move(metadata), {
            BoolSetting("show-input", "Show key presses and clicks", true),
            BoolSetting("show-distance", "Show distance travelled", true,
                "Needs validated local player position; shows -- until it is available.")}, layout) {}
    void OnSettingsChanged(const SettingsReader& settings) override {
        std::scoped_lock lock(mutex_);
        showInput_ = settings.Bool("show-input");
        showDistance_ = settings.Bool("show-distance");
    }
    void OnEnable() override { std::scoped_lock lock(mutex_); counters_.Reset(SteadyClock::now()); }
    void OnKeyboard(const KeyboardEvent& event) override {
        if (event.uiCaptured || !event.pressed || event.repeated) return;
        std::scoped_lock lock(mutex_);
        ++counters_.keyPresses;
    }
    void OnMouse(const MouseEvent& event) override {
        if (event.uiCaptured || !event.pressed) return;
        std::scoped_lock lock(mutex_);
        if (event.changedButton == 1U) ++counters_.leftClicks;
        else if (event.changedButton == 2U) ++counters_.rightClicks;
    }
    void OnPlayerTransform(const PlayerTransform& transform) override {
        std::scoped_lock lock(mutex_);
        counters_.AddPosition(transform);
    }
    void OnRender(const RenderEvent& event) override { std::scoped_lock lock(mutex_); NoteRender(event); }
    [[nodiscard]] std::vector<ModuleAction> Actions() const override { return {{"reset", "Reset counters"}}; }
    bool InvokeAction(std::string_view action, SteadyTime now) override {
        if (action != "reset") return false;
        std::scoped_lock lock(mutex_);
        counters_.Reset(now);
        return true;
    }
    [[nodiscard]] HudContent Content() const override {
        std::scoped_lock lock(mutex_);
        HudContent content;
        const auto elapsed = RenderNow() > counters_.startedAt
            ? std::chrono::duration_cast<std::chrono::milliseconds>(RenderNow() - counters_.startedAt)
            : std::chrono::milliseconds::zero();
        content.lines.push_back({"Session  " + FormatDuration(elapsed), HudTone::Normal});
        if (showInput_) {
            content.lines.push_back({std::format("Keys {}  L {}  R {}", counters_.keyPresses, counters_.leftClicks,
                counters_.rightClicks), HudTone::Normal});
        }
        if (showDistance_) {
            content.lines.push_back(counters_.lastPosition
                ? HudLine{std::format("Travelled  {:.0f} blocks", counters_.distanceBlocks), HudTone::Normal}
                : HudLine{"Travelled  --", HudTone::Muted});
        }
        return content;
    }

private:
    SessionCounters counters_{};
    bool showInput_{true};
    bool showDistance_{true};
};

class ArmorStatusModule final : public HudModuleBase<> {
public:
    ArmorStatusModule(ModuleMetadata metadata, HudLayout layout, SharedServices&)
        : HudModuleBase(std::move(metadata), {
            ChoiceSetting("display", "Durability as", "percent", DurabilityModes()),
            BoolSetting("show-empty", "Show empty slots", false),
            BoolSetting("show-names", "Show item names", true),
            IntSetting("warn-percent", "Highlight at or below", 20, 1, 100, "%d%%")}, layout) {}
    void OnSettingsChanged(const SettingsReader& settings) override {
        std::scoped_lock lock(mutex_);
        mode_ = settings.Choice("display");
        showEmpty_ = settings.Bool("show-empty");
        showNames_ = settings.Bool("show-names");
        warnPercent_ = settings.Int("warn-percent");
    }
    void OnDisable() override { std::scoped_lock lock(mutex_); equipment_.reset(); }
    void OnEquipment(const EquipmentSnapshot& snapshot) override { std::scoped_lock lock(mutex_); equipment_ = snapshot; }
    [[nodiscard]] HudContent Content() const override {
        std::scoped_lock lock(mutex_);
        HudContent content;
        if (!equipment_) return {{{"Armor  --", HudTone::Muted}}, {}, {}};
        for (std::size_t index = 0; index < ArmorSlotCount; ++index) {
            const auto slot = static_cast<ArmorSlot>(index);
            const auto& item = equipment_->armor[index];
            if (!item) {
                if (showEmpty_) content.lines.push_back({std::string(ArmorSlotName(slot)) + "  empty", HudTone::Muted});
                continue;
            }
            const auto label = showNames_ ? ItemName(*item) : std::string(ArmorSlotName(slot));
            content.lines.push_back({label + "  " + DurabilityText(*item, mode_), DurabilityTone(*item, warnPercent_)});
        }
        if (content.lines.empty()) content.lines.push_back({"Armor  none", HudTone::Muted});
        return content;
    }

private:
    std::optional<EquipmentSnapshot> equipment_;
    std::string mode_{"percent"};
    bool showEmpty_{};
    bool showNames_{true};
    int warnPercent_{20};
};

class HeldItemDurabilityModule final : public HudModuleBase<> {
public:
    HeldItemDurabilityModule(ModuleMetadata metadata, HudLayout layout, SharedServices&)
        : HudModuleBase(std::move(metadata), {
            ChoiceSetting("display", "Durability as", "both", DurabilityModes()),
            BoolSetting("include-offhand", "Include off-hand item", false),
            BoolSetting("hide-undamageable", "Hide items without durability", true),
            IntSetting("warn-percent", "Highlight at or below", 20, 1, 100, "%d%%")}, layout) {}
    void OnSettingsChanged(const SettingsReader& settings) override {
        std::scoped_lock lock(mutex_);
        mode_ = settings.Choice("display");
        offhand_ = settings.Bool("include-offhand");
        hideUndamageable_ = settings.Bool("hide-undamageable");
        warnPercent_ = settings.Int("warn-percent");
    }
    void OnDisable() override { std::scoped_lock lock(mutex_); equipment_.reset(); }
    void OnEquipment(const EquipmentSnapshot& snapshot) override { std::scoped_lock lock(mutex_); equipment_ = snapshot; }
    [[nodiscard]] HudContent Content() const override {
        std::scoped_lock lock(mutex_);
        HudContent content;
        if (!equipment_) return {{{"Held  --", HudTone::Muted}}, {}, {}};
        const auto add = [&](const std::optional<ItemStack>& item) {
            if (!item) return;
            const bool damageable = DurabilityPercent(*item).has_value();
            if (!damageable && hideUndamageable_) return;
            content.lines.push_back({ItemName(*item) + "  " + DurabilityText(*item, mode_), DurabilityTone(*item, warnPercent_)});
        };
        add(equipment_->mainHand);
        if (offhand_) add(equipment_->offHand);
        if (content.lines.empty()) content.lines.push_back({"Held  --", HudTone::Muted});
        return content;
    }

private:
    std::optional<EquipmentSnapshot> equipment_;
    std::string mode_{"both"};
    bool offhand_{};
    bool hideUndamageable_{true};
    int warnPercent_{20};
};

class ItemCounterModule final : public HudModuleBase<> {
public:
    ItemCounterModule(ModuleMetadata metadata, HudLayout layout, SharedServices&)
        : HudModuleBase(std::move(metadata), {
            ChoiceSetting("count", "Count", "inventory", {{"inventory", "Whole inventory"}, {"stack", "Selected stack only"}}),
            BoolSetting("show-name", "Show item name", true)}, layout) {}
    void OnSettingsChanged(const SettingsReader& settings) override {
        std::scoped_lock lock(mutex_);
        wholeInventory_ = settings.Choice("count") == "inventory";
        showName_ = settings.Bool("show-name");
    }
    void OnDisable() override { std::scoped_lock lock(mutex_); inventory_.reset(); }
    void OnInventory(const InventorySnapshot& snapshot) override {
        std::scoped_lock lock(mutex_);
        inventory_ = snapshot;
        if (inventory_->slots.size() > MaxInventorySlots) inventory_->slots.resize(MaxInventorySlots);
    }
    [[nodiscard]] HudContent Content() const override {
        std::scoped_lock lock(mutex_);
        const ItemStack* selected = inventory_ ? SelectedItem(*inventory_) : nullptr;
        if (!selected) return {{{"Items  --", HudTone::Muted}}, {}, {}};
        const int count = wholeInventory_ ? CountItem(*inventory_, selected->itemId) : selected->count;
        const auto label = showName_ ? ItemName(*selected) : std::string("Items");
        return {{{std::format("{}  x{}", label, count), HudTone::Normal}}, {}, {}};
    }

private:
    std::optional<InventorySnapshot> inventory_;
    bool wholeInventory_{true};
    bool showName_{true};
};

class PotionEffectsModule final : public HudModuleBase<> {
public:
    PotionEffectsModule(ModuleMetadata metadata, HudLayout layout, SharedServices&)
        : HudModuleBase(std::move(metadata), {
            IntSetting("max-shown", "Effects shown", 6, 1, 12),
            BoolSetting("show-level", "Show level (II, III ...)", true),
            ChoiceSetting("sort", "Sort by", "remaining", {{"remaining", "Time remaining"}, {"name", "Name"}}),
            IntSetting("warn-seconds", "Highlight below", 10, 0, 120, "%d s")}, layout) {}
    void OnSettingsChanged(const SettingsReader& settings) override {
        std::scoped_lock lock(mutex_);
        maxShown_ = static_cast<std::size_t>(settings.Int("max-shown"));
        showLevel_ = settings.Bool("show-level");
        sortByName_ = settings.Choice("sort") == "name";
        warnBelow_ = std::chrono::seconds(settings.Int("warn-seconds"));
    }
    void OnDisable() override { std::scoped_lock lock(mutex_); effects_.reset(); }
    void OnEffects(const EffectsSnapshot& snapshot) override { std::scoped_lock lock(mutex_); effects_ = snapshot; }
    [[nodiscard]] HudContent Content() const override {
        std::scoped_lock lock(mutex_);
        if (!effects_) return {{{"Effects  --", HudTone::Muted}}, {}, {}};
        if (effects_->effects.empty()) return {{{"Effects  none", HudTone::Muted}}, {}, {}};
        auto sorted = effects_->effects;
        if (sortByName_) {
            std::ranges::sort(sorted, {}, &StatusEffect::displayName);
        } else {
            std::ranges::stable_sort(sorted, [](const StatusEffect& a, const StatusEffect& b) {
                const auto left = a.remaining.value_or(std::chrono::milliseconds::max());
                const auto right = b.remaining.value_or(std::chrono::milliseconds::max());
                return left < right;
            });
        }
        HudContent content;
        for (const auto& effect : sorted) {
            if (content.lines.size() >= maxShown_) {
                content.lines.push_back({std::format("+{} more", sorted.size() - maxShown_), HudTone::Muted});
                break;
            }
            const bool expiring = effect.remaining && *effect.remaining <= warnBelow_;
            content.lines.push_back({FormatStatusEffect(effect, showLevel_), expiring ? HudTone::Warning : HudTone::Normal});
        }
        return content;
    }

private:
    std::optional<EffectsSnapshot> effects_;
    std::size_t maxShown_{6};
    bool showLevel_{true};
    bool sortByName_{};
    std::chrono::milliseconds warnBelow_{std::chrono::seconds(10)};
};

class ServerDisplayModule final : public HudModuleBase<> {
public:
    ServerDisplayModule(ModuleMetadata metadata, HudLayout layout, SharedServices&)
        : HudModuleBase(std::move(metadata), {
            BoolSetting("show-kind", "Show session type", true),
            BoolSetting("show-addresses", "Show address-like names", false,
                "Off: names that look like an IP or hostname are masked, e.g. while streaming.")}, layout) {}
    void OnSettingsChanged(const SettingsReader& settings) override {
        std::scoped_lock lock(mutex_);
        showKind_ = settings.Bool("show-kind");
        showAddresses_ = settings.Bool("show-addresses");
    }
    void OnDisable() override { std::scoped_lock lock(mutex_); state_.reset(); }
    void OnServerState(const ServerState& state) override { std::scoped_lock lock(mutex_); state_ = state; }
    [[nodiscard]] HudContent Content() const override {
        std::scoped_lock lock(mutex_);
        if (!state_) return {{{"Server  --", HudTone::Muted}}, {}, {}};
        if (state_->kind == SessionKind::None) return {{{"Server  not in a world", HudTone::Muted}}, {}, {}};
        std::string name = state_->displayName.empty() ? std::string(SessionKindName(state_->kind))
            : !showAddresses_ && LooksLikeAddress(state_->displayName) ? std::string("hidden address")
            : Ellipsize(ToUtf8(state_->displayName), 40);
        std::string text = (showKind_ && !state_->displayName.empty())
            ? std::format("{}  {}", SessionKindName(state_->kind), name) : "Server  " + name;
        return {{{text, HudTone::Normal}}, {}, {}};
    }

private:
    std::optional<ServerState> state_;
    bool showKind_{true};
    bool showAddresses_{};
};

class ResourcePackDisplayModule final : public HudModuleBase<> {
public:
    ResourcePackDisplayModule(ModuleMetadata metadata, HudLayout layout, SharedServices&)
        : HudModuleBase(std::move(metadata), {
            IntSetting("max-shown", "Packs shown", 1, 1, 6, "%d", "Highest-priority pack first."),
            BoolSetting("show-version", "Show pack version", false)}, layout) {}
    void OnSettingsChanged(const SettingsReader& settings) override {
        std::scoped_lock lock(mutex_);
        maxShown_ = static_cast<std::size_t>(settings.Int("max-shown"));
        showVersion_ = settings.Bool("show-version");
    }
    void OnDisable() override { std::scoped_lock lock(mutex_); state_.reset(); }
    void OnResourcePacks(const ResourcePackState& state) override { std::scoped_lock lock(mutex_); state_ = state; }
    [[nodiscard]] HudContent Content() const override {
        std::scoped_lock lock(mutex_);
        if (!state_) return {{{"Pack  --", HudTone::Muted}}, {}, {}};
        if (state_->activePacks.empty()) return {{{"Pack  Default", HudTone::Normal}}, {}, {}};
        HudContent content;
        for (const auto& pack : state_->activePacks) {
            if (content.lines.size() >= maxShown_) {
                content.lines.push_back({std::format("+{} more", state_->activePacks.size() - maxShown_), HudTone::Muted});
                break;
            }
            std::string text = (content.lines.empty() ? "Pack  " : "      ") + Ellipsize(ToUtf8(pack.name), 36);
            if (showVersion_ && !pack.version.empty()) text += "  " + Ellipsize(ToUtf8(pack.version), 12);
            content.lines.push_back({text, HudTone::Normal});
        }
        return content;
    }

private:
    std::optional<ResourcePackState> state_;
    std::size_t maxShown_{1};
    bool showVersion_{};
};
}

std::unique_ptr<IModule> MakeClock(ModuleMetadata m, HudLayout l, SharedServices& s) { return std::make_unique<ClockModule>(std::move(m), l, s); }
std::unique_ptr<IModule> MakeStopwatch(ModuleMetadata m, HudLayout l, SharedServices& s) { return std::make_unique<StopwatchModule>(std::move(m), l, s); }
std::unique_ptr<IModule> MakeSessionStatistics(ModuleMetadata m, HudLayout l, SharedServices& s) { return std::make_unique<SessionStatisticsModule>(std::move(m), l, s); }
std::unique_ptr<IModule> MakeArmorStatus(ModuleMetadata m, HudLayout l, SharedServices& s) { return std::make_unique<ArmorStatusModule>(std::move(m), l, s); }
std::unique_ptr<IModule> MakeHeldItemDurability(ModuleMetadata m, HudLayout l, SharedServices& s) { return std::make_unique<HeldItemDurabilityModule>(std::move(m), l, s); }
std::unique_ptr<IModule> MakeItemCounter(ModuleMetadata m, HudLayout l, SharedServices& s) { return std::make_unique<ItemCounterModule>(std::move(m), l, s); }
std::unique_ptr<IModule> MakePotionEffects(ModuleMetadata m, HudLayout l, SharedServices& s) { return std::make_unique<PotionEffectsModule>(std::move(m), l, s); }
std::unique_ptr<IModule> MakeServerDisplay(ModuleMetadata m, HudLayout l, SharedServices& s) { return std::make_unique<ServerDisplayModule>(std::move(m), l, s); }
std::unique_ptr<IModule> MakeResourcePackDisplay(ModuleMetadata m, HudLayout l, SharedServices& s) { return std::make_unique<ResourcePackDisplayModule>(std::move(m), l, s); }

}
