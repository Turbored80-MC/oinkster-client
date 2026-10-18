#include "Oinkster/Modules/Factories.hpp"
#include "Oinkster/Modules/InputLogic.hpp"
#include <Windows.h>
#include <algorithm>
#include <array>
#include <bitset>
#include <format>
#include <optional>

namespace Oinkster::Modules {

void ClickDebouncer::Configure(std::chrono::milliseconds left, std::chrono::milliseconds right, bool linked) noexcept {
    left_ = std::max(left, std::chrono::milliseconds(10));
    right_ = linked ? left_ : std::max(right, std::chrono::milliseconds(10));
}

bool ClickDebouncer::Allow(std::uint32_t button, SteadyTime at) noexcept {
    auto* last = button == 1U ? &lastLeft_ : button == 2U ? &lastRight_ : nullptr;
    const auto interval = button == 1U ? left_ : right_;
    if (!last) return true;
    if (*last && at >= **last && at - **last < interval) return false;
    *last = at;
    return true;
}

void NullMovementFilter::Configure(std::array<Axis, 2> axes) noexcept {
    axes_ = axes;
    Reset();
}

bool NullMovementFilter::Transform(std::uint32_t key, bool pressed, bool repeated,
    const std::array<bool, 256>& physical, std::vector<KeyMessage>& out) {
    if (key == 0 || key >= 256) return false;
    std::uint32_t other = 0;
    for (const auto& axis : axes_) {
        if (axis.first == 0 || axis.second == 0 || axis.first == axis.second) continue;
        if (key == axis.first) other = axis.second;
        else if (key == axis.second) other = axis.first;
    }
    if (other == 0 || other >= 256) return false;
    if (pressed) {
        // Auto-repeat only continues a key the game already holds.
        if (repeated) {
            if (sent_[key]) out.push_back({key, true});
            return true;
        }
        if (sent_[other]) {
            out.push_back({other, false});
            sent_[other] = false;
        }
        out.push_back({key, true});
        sent_[key] = true;
        return true;
    }
    if (sent_[key]) {
        out.push_back({key, false});
        sent_[key] = false;
    }
    if (physical[other] && !sent_[other]) {
        out.push_back({other, true});
        sent_[other] = true;
    }
    return true;
}

void NullMovementFilter::Reset() noexcept { sent_ = {}; }

void ClickDebouncer::Clear() noexcept {
    lastLeft_.reset();
    lastRight_.reset();
}

void CpsLimiter::Configure(std::size_t left, std::size_t right) noexcept {
    leftLimit_ = std::clamp<std::size_t>(left, 1, 30);
    rightLimit_ = std::clamp<std::size_t>(right, 1, 30);
}

bool CpsLimiter::Allow(std::uint32_t button, SteadyTime at) noexcept {
    auto* window = button == 1U ? &left_ : button == 2U ? &right_ : nullptr;
    auto* last = button == 1U ? &lastLeft_ : button == 2U ? &lastRight_ : nullptr;
    const auto limit = button == 1U ? leftLimit_ : rightLimit_;
    if (!window || !last) return true;
    if (*last && at < **last) window->Clear();
    *last = at;
    if (window->Count(at, std::chrono::seconds(1)) >= limit) return false;
    window->Add(at);
    return true;
}

void CpsLimiter::Clear() noexcept {
    left_.Clear();
    right_.Clear();
    lastLeft_.reset();
    lastRight_.reset();
}

void InputLatencyMeter::OnInput(SteadyTime at) {
    if (!pending_ || at < *pending_) pending_ = at;
}

void InputLatencyMeter::OnFrame(SteadyTime presentedAt) {
    if (!pending_ || presentedAt < *pending_) return;
    const double milliseconds = std::chrono::duration<double, std::milli>(presentedAt - *pending_).count();
    pending_.reset();
    if (milliseconds <= MaxPlausibleMilliseconds) samples_.Push(milliseconds);
}

void InputLatencyMeter::Clear() {
    samples_.Clear();
    pending_.reset();
}

std::optional<double> InputLatencyMeter::LastMilliseconds() const {
    return samples_.Empty() ? std::nullopt : std::optional(samples_.Back());
}

std::optional<double> InputLatencyMeter::AverageMilliseconds() const {
    if (samples_.Empty()) return std::nullopt;
    double total{};
    for (std::size_t index = 0; index < samples_.Size(); ++index) total += samples_.At(index);
    return total / static_cast<double>(samples_.Size());
}

std::optional<double> InputLatencyMeter::MaximumMilliseconds() const {
    if (samples_.Empty()) return std::nullopt;
    double peak{};
    for (std::size_t index = 0; index < samples_.Size(); ++index) peak = std::max(peak, samples_.At(index));
    return peak;
}

std::string ShortKeyLabel(std::uint32_t key) {
    if ((key >= '0' && key <= '9') || (key >= 'A' && key <= 'Z')) return std::string(1, static_cast<char>(key));
    switch (key) {
    case VK_SPACE: return "Space";
    case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT: return "Shift";
    case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL: return "Ctrl";
    case VK_MENU: case VK_LMENU: case VK_RMENU: return "Alt";
    case VK_TAB: return "Tab";
    case VK_UP: return "Up";
    case VK_DOWN: return "Down";
    case VK_LEFT: return "Left";
    case VK_RIGHT: return "Right";
    default: break;
    }
    return std::format("#{}", key);
}

namespace {
using std::chrono::seconds;

// Keyboard events for VK_SHIFT/VK_CONTROL arrive as the generic code from
// WM_KEYDOWN; normalize left/right variants so either matches a setting.
std::uint32_t NormalizeKey(std::uint32_t key) noexcept {
    switch (key) {
    case VK_LSHIFT: case VK_RSHIFT: return VK_SHIFT;
    case VK_LCONTROL: case VK_RCONTROL: return VK_CONTROL;
    case VK_LMENU: case VK_RMENU: return VK_MENU;
    default: return key;
    }
}

class CpsModule final : public HudModuleBase<> {
public:
    CpsModule(ModuleMetadata metadata, HudLayout layout, SharedServices&)
        : HudModuleBase(std::move(metadata), {
            BoolSetting("show-right", "Show right clicks", true),
            ChoiceSetting("count-mode", "Count", "physical", {
                {"physical", "Physical clicks"}, {"accepted", "Clicks accepted by filters"}},
                "Physical preserves raw device telemetry; accepted excludes clicks discarded by DC Prevent or CPS Cap."),
            ChoiceSetting("style", "Style", "labelled", {{"labelled", "CPS  L 9  R 2"}, {"compact", "9 | 2"}})}, layout) {}
    void OnSettingsChanged(const SettingsReader& settings) override {
        std::scoped_lock lock(mutex_);
        showRight_ = settings.Bool("show-right");
        acceptedOnly_ = settings.Choice("count-mode") == "accepted";
        compact_ = settings.Choice("style") == "compact";
    }
    void OnEnable() override { Reset(); }
    void OnDisable() override { Reset(); }
    void OnMouse(const MouseEvent& event) override {
        // Clicks the ClickGUI consumed never reached gameplay - not counted.
        if (event.uiCaptured || !event.pressed) return;
        std::scoped_lock lock(mutex_);
        if (acceptedOnly_ && event.IsCancelled()) return;
        if (event.changedButton == 1U) left_.Add(event.occurredAt);
        else if (event.changedButton == 2U) right_.Add(event.occurredAt);
    }
    void OnRender(const RenderEvent& event) override {
        std::scoped_lock lock(mutex_);
        NoteRender(event);
        leftCount_ = left_.Count(event.presentedAt, seconds(1));
        rightCount_ = right_.Count(event.presentedAt, seconds(1));
    }
    [[nodiscard]] HudContent Content() const override {
        std::scoped_lock lock(mutex_);
        std::string text = compact_
            ? (showRight_ ? std::format("{} | {}", leftCount_, rightCount_) : std::format("{} CPS", leftCount_))
            : (showRight_ ? std::format("CPS  L {}  R {}", leftCount_, rightCount_) : std::format("CPS  {}", leftCount_));
        return {{{text, HudTone::Normal}}, {}, {}};
    }

private:
    void Reset() {
        std::scoped_lock lock(mutex_);
        left_.Clear();
        right_.Clear();
        leftCount_ = rightCount_ = 0;
    }
    EventRateWindow left_{64};
    EventRateWindow right_{64};
    std::size_t leftCount_{};
    std::size_t rightCount_{};
    bool showRight_{true};
    bool acceptedOnly_{};
    bool compact_{};
};

class KeystrokesModule final : public HudModuleBase<> {
public:
    KeystrokesModule(ModuleMetadata metadata, HudLayout layout, SharedServices&)
        : HudModuleBase(std::move(metadata), {
            KeySetting("forward-key", "Forward key", 'W'),
            KeySetting("left-key", "Left key", 'A'),
            KeySetting("back-key", "Back key", 'S'),
            KeySetting("right-key", "Right key", 'D'),
            BoolSetting("show-mouse", "Show mouse buttons", true),
            BoolSetting("show-cps", "Show CPS on mouse buttons", true),
            BoolSetting("show-jump", "Show jump (Space)", true),
            BoolSetting("show-sneak-sprint", "Show sneak and sprint (Shift / Ctrl)", false)}, layout) {}
    void OnSettingsChanged(const SettingsReader& settings) override {
        std::scoped_lock lock(mutex_);
        movement_ = {settings.Key("forward-key"), settings.Key("left-key"), settings.Key("back-key"), settings.Key("right-key")};
        showMouse_ = settings.Bool("show-mouse");
        showCps_ = settings.Bool("show-cps");
        showJump_ = settings.Bool("show-jump");
        showModifiers_ = settings.Bool("show-sneak-sprint");
    }
    void OnEnable() override { ClearState(); ClearClicks(); }
    void OnDisable() override { ClearState(); ClearClicks(); }
    void OnKeyboard(const KeyboardEvent& event) override {
        if (event.repeated) return;
        // A press the ClickGUI owns never reached the game; releases always
        // apply (the menu-open path publishes releases for held keys).
        if (event.pressed && event.uiCaptured) return;
        const auto key = NormalizeKey(event.key);
        if (key >= down_.size()) return;
        std::scoped_lock lock(mutex_);
        down_[key] = event.pressed;
    }
    void OnMouse(const MouseEvent& event) override {
        if (event.changedButton != 1U && event.changedButton != 2U) return;
        if (event.pressed && (event.uiCaptured || event.IsCancelled())) return;
        std::scoped_lock lock(mutex_);
        const bool left = event.changedButton == 1U;
        (left ? leftDown_ : rightDown_) = event.pressed;
        if (event.pressed) {
            (left ? leftClicks_ : rightClicks_).Add(event.occurredAt);
            (left ? lastLeftMessage_ : lastRightMessage_) = event.occurredAt;
        }
    }
    void OnRender(const RenderEvent& event) override {
        // Never show keys for input that went to another application.
        if (!event.gameFocused) ClearState();
        std::scoped_lock lock(mutex_);
        // While the ClickGUI is open its presses never reach the game, so the
        // message path (which knows what the UI captured) stays in charge.
        if (event.keysDown && event.gameFocused && !event.menuVisible) ApplyPhysicalState(*event.keysDown, event.presentedAt);
        leftCps_ = leftClicks_.Count(event.presentedAt, seconds(1));
        rightCps_ = rightClicks_.Count(event.presentedAt, seconds(1));
    }
    [[nodiscard]] HudContent Content() const override {
        std::scoped_lock lock(mutex_);
        const auto key = [&](std::uint32_t code, float width = 1.0F) {
            HudKey result;
            result.label = code == 0 ? std::string("-") : ShortKeyLabel(code);
            result.pressed = code != 0 && code < down_.size() && down_[code];
            result.widthUnits = width;
            return result;
        };
        const auto mouse = [&](const char* label, bool pressed, std::size_t cps) {
            HudKey result{label, pressed, 1.5F};
            if (showCps_) result.subLabel = std::format("{} CPS", cps);
            return result;
        };
        HudKey spacer;
        spacer.spacer = true;
        // A three-column grid: every row spans three key widths so the
        // renderer can lay them out flush, with W centered above S.
        HudContent content;
        content.bare = true;
        content.keyRows.push_back({spacer, key(movement_[0]), spacer});
        content.keyRows.push_back({key(movement_[1]), key(movement_[2]), key(movement_[3])});
        if (showMouse_) content.keyRows.push_back({mouse("LMB", leftDown_, leftCps_), mouse("RMB", rightDown_, rightCps_)});
        if (showJump_) {
            auto space = key(VK_SPACE, 3.0F);
            space.bar = true;
            content.keyRows.push_back({space});
        }
        if (showModifiers_) content.keyRows.push_back({key(VK_SHIFT, 1.5F), key(VK_CONTROL, 1.5F)});
        return content;
    }

private:
    // Minecraft reads gameplay input via raw input / GameInput, so with the
    // cursor captured the window often never sees WM_KEYDOWN or button
    // messages. The per-frame physical snapshot is authoritative for what is
    // held; it only counts clicks for a button whose messages have gone
    // quiet, so a click is never counted by both paths.
    void ApplyPhysicalState(const std::bitset<256>& keys, SteadyTime at) {
        for (std::size_t key = 0; key < down_.size(); ++key) down_[key] = keys[key];
        const auto apply = [&](bool pressed, bool& down, EventRateWindow& clicks, std::optional<SteadyTime> lastMessage) {
            const bool messagesActive = lastMessage && at >= *lastMessage && at - *lastMessage < seconds(1);
            if (pressed && !down && !messagesActive) clicks.Add(at);
            down = pressed;
        };
        apply(keys[VK_LBUTTON], leftDown_, leftClicks_, lastLeftMessage_);
        apply(keys[VK_RBUTTON], rightDown_, rightClicks_, lastRightMessage_);
    }
    void ClearState() {
        std::scoped_lock lock(mutex_);
        down_.fill(false);
        leftDown_ = rightDown_ = false;
    }
    void ClearClicks() {
        std::scoped_lock lock(mutex_);
        leftClicks_.Clear();
        rightClicks_.Clear();
        leftCps_ = rightCps_ = 0;
        lastLeftMessage_.reset();
        lastRightMessage_.reset();
    }
    std::array<bool, 256> down_{};
    std::array<std::uint32_t, 4> movement_{'W', 'A', 'S', 'D'};
    EventRateWindow leftClicks_{64};
    EventRateWindow rightClicks_{64};
    std::size_t leftCps_{};
    std::size_t rightCps_{};
    bool leftDown_{};
    bool rightDown_{};
    std::optional<SteadyTime> lastLeftMessage_;
    std::optional<SteadyTime> lastRightMessage_;
    bool showMouse_{true};
    bool showCps_{true};
    bool showJump_{true};
    bool showModifiers_{};
};

class InputLatencyModule final : public HudModuleBase<> {
public:
    InputLatencyModule(ModuleMetadata metadata, HudLayout layout, SharedServices&)
        : HudModuleBase(std::move(metadata), {
            ChoiceSetting("display", "Show", "average", {
                {"average", "Rolling average"}, {"last", "Last sample"}, {"both", "Average and peak"}},
                "Input to frame: window message received -> next Present call. Excludes device, GPU and display latency.")}, layout) {}
    void OnSettingsChanged(const SettingsReader& settings) override {
        std::scoped_lock lock(mutex_);
        display_ = settings.Choice("display");
    }
    void OnEnable() override { std::scoped_lock lock(mutex_); meter_.Clear(); }
    void OnKeyboard(const KeyboardEvent& event) override {
        if (event.uiCaptured || !event.pressed || event.repeated) return;
        std::scoped_lock lock(mutex_);
        meter_.OnInput(event.occurredAt);
    }
    void OnMouse(const MouseEvent& event) override {
        if (event.uiCaptured || !((event.pressed && event.changedButton != 0) || event.wheel != 0.0F)) return;
        std::scoped_lock lock(mutex_);
        meter_.OnInput(event.occurredAt);
    }
    void OnRender(const RenderEvent& event) override {
        std::scoped_lock lock(mutex_);
        NoteRender(event);
        meter_.OnFrame(event.presentedAt);
    }
    [[nodiscard]] HudContent Content() const override {
        std::scoped_lock lock(mutex_);
        const auto format = [](std::optional<double> value) {
            return value ? std::format("{:.1f} ms", *value) : std::string("--");
        };
        std::string text;
        if (display_ == "last") text = "Input to frame  " + format(meter_.LastMilliseconds());
        else if (display_ == "both") text = "Input to frame  " + format(meter_.AverageMilliseconds()) + "  peak " + format(meter_.MaximumMilliseconds());
        else text = "Input to frame  " + format(meter_.AverageMilliseconds());
        return {{{text, meter_.SampleCount() == 0 ? HudTone::Muted : HudTone::Normal}}, {}, {}};
    }

private:
    InputLatencyMeter meter_;
    std::string display_{"average"};
};

class DcPreventModule final : public HudModuleBase<> {
public:
    DcPreventModule(ModuleMetadata metadata, HudLayout layout, SharedServices&)
        : HudModuleBase(std::move(metadata), {
            IntSetting("left-ms", "Left debounce", 20, 10, 1000, "%d ms",
                "Clicks inside this interval are discarded; the interval can never be shorter than 10 ms."),
            IntSetting("right-ms", "Right debounce", 20, 10, 1000, "%d ms"),
            BoolSetting("linked", "Link left and right", true)}, layout) {}
    void OnSettingsChanged(const SettingsReader& settings) override {
        std::scoped_lock lock(mutex_);
        filter_.Configure(std::chrono::milliseconds(settings.Int("left-ms")),
            std::chrono::milliseconds(settings.Int("right-ms")), settings.Bool("linked"));
    }
    void OnEnable() override { Reset(); }
    void OnDisable() override { Reset(); }
    void OnMouse(const MouseEvent& event) override {
        if (event.uiCaptured || event.IsCancelled() || !event.pressed
            || (event.changedButton != 1U && event.changedButton != 2U)) return;
        std::scoped_lock lock(mutex_);
        if (filter_.Allow(event.changedButton, event.occurredAt)) return;
        ++suppressed_;
        event.Cancel();
    }
    [[nodiscard]] HudContent Content() const override {
        std::scoped_lock lock(mutex_);
        return {{{std::format("DC Prevent  {} suppressed", suppressed_),
            suppressed_ == 0 ? HudTone::Muted : HudTone::Good}}, {}, {}};
    }

private:
    void Reset() {
        std::scoped_lock lock(mutex_);
        filter_.Clear();
        suppressed_ = 0;
    }
    ClickDebouncer filter_;
    std::uint64_t suppressed_{};
};

class CpsCapModule final : public HudModuleBase<> {
public:
    CpsCapModule(ModuleMetadata metadata, HudLayout layout, SharedServices&)
        : HudModuleBase(std::move(metadata), {
            IntSetting("left-cps", "Left CPS limit", 15, 1, 30, "%d CPS"),
            IntSetting("right-cps", "Right CPS limit", 15, 1, 30, "%d CPS")}, layout) {}
    void OnSettingsChanged(const SettingsReader& settings) override {
        std::scoped_lock lock(mutex_);
        filter_.Configure(static_cast<std::size_t>(settings.Int("left-cps")),
            static_cast<std::size_t>(settings.Int("right-cps")));
    }
    void OnEnable() override { Reset(); }
    void OnDisable() override { Reset(); }
    void OnMouse(const MouseEvent& event) override {
        // DC Prevent is registered first at the same Immediate priority. A
        // click it rejects never consumes CPS Cap's rolling-window budget.
        if (event.uiCaptured || event.IsCancelled() || !event.pressed
            || (event.changedButton != 1U && event.changedButton != 2U)) return;
        std::scoped_lock lock(mutex_);
        if (filter_.Allow(event.changedButton, event.occurredAt)) return;
        ++suppressed_;
        event.Cancel();
    }
    [[nodiscard]] HudContent Content() const override {
        std::scoped_lock lock(mutex_);
        return {{{std::format("CPS Cap  {} suppressed", suppressed_),
            suppressed_ == 0 ? HudTone::Muted : HudTone::Good}}, {}, {}};
    }

private:
    void Reset() {
        std::scoped_lock lock(mutex_);
        filter_.Clear();
        suppressed_ = 0;
    }
    CpsLimiter filter_;
    std::uint64_t suppressed_{};
};
}

std::unique_ptr<IModule> MakeCps(ModuleMetadata m, HudLayout l, SharedServices& s) { return std::make_unique<CpsModule>(std::move(m), l, s); }
std::unique_ptr<IModule> MakeKeystrokes(ModuleMetadata m, HudLayout l, SharedServices& s) { return std::make_unique<KeystrokesModule>(std::move(m), l, s); }
std::unique_ptr<IModule> MakeInputLatency(ModuleMetadata m, HudLayout l, SharedServices& s) { return std::make_unique<InputLatencyModule>(std::move(m), l, s); }
std::unique_ptr<IModule> MakeDcPrevent(ModuleMetadata m, HudLayout l, SharedServices& s) { return std::make_unique<DcPreventModule>(std::move(m), l, s); }
std::unique_ptr<IModule> MakeCpsCap(ModuleMetadata m, HudLayout l, SharedServices& s) { return std::make_unique<CpsCapModule>(std::move(m), l, s); }

}
