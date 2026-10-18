#include "Oinkster/Modules/Factories.hpp"
#include "Oinkster/Modules/AccessibilityLogic.hpp"
#include "Oinkster/Modules/HudFormat.hpp"
#include "Oinkster/Modules/TelemetryLogic.hpp"
#include <Windows.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <ctime>
#include <cwctype>
#include <format>
#include <vector>

namespace Oinkster::Modules {

RelativeDirection DirectionTo(const Vec3& listener, float yawDegrees, const Vec3& source) noexcept {
    if (!std::isfinite(yawDegrees)) return RelativeDirection::Unknown;
    const double dx = source.x - listener.x;
    const double dz = source.z - listener.z;
    if (std::abs(dx) < 1e-6 && std::abs(dz) < 1e-6) return RelativeDirection::Unknown;
    constexpr double Radians = 3.14159265358979323846 / 180.0;
    const double yaw = static_cast<double>(NormalizeYaw360(yawDegrees)) * Radians;
    // Bedrock: forward = (-sin yaw, cos yaw); right = (-cos yaw, -sin yaw).
    const double forward = dx * -std::sin(yaw) + dz * std::cos(yaw);
    const double right = dx * -std::cos(yaw) + dz * -std::sin(yaw);
    const double angle = std::atan2(right, forward) / Radians;  // 0 ahead, +90 right
    if (std::abs(angle) <= 45.0) return RelativeDirection::Ahead;
    if (std::abs(angle) >= 135.0) return RelativeDirection::Behind;
    return angle > 0.0 ? RelativeDirection::Right : RelativeDirection::Left;
}

std::string_view DirectionGlyph(RelativeDirection direction) noexcept {
    switch (direction) {
    case RelativeDirection::Ahead: return "^";
    case RelativeDirection::Behind: return "v";
    case RelativeDirection::Left: return "<";
    case RelativeDirection::Right: return ">";
    case RelativeDirection::Unknown: break;
    }
    return " ";
}

std::string CaptionForSound(std::string_view soundId) {
    if (soundId.empty() || soundId.size() > 96) return {};
    for (const std::string_view skipped : {"music.", "record.", "ui.", "random.click", "random.pop2"}) {
        if (soundId.starts_with(skipped)) return {};
    }
    std::vector<std::string_view> parts;
    for (std::size_t start = 0; start <= soundId.size();) {
        const auto dot = soundId.find('.', start);
        const auto end = dot == std::string_view::npos ? soundId.size() : dot;
        if (end > start) parts.push_back(soundId.substr(start, end - start));
        start = end + 1;
    }
    // Category prefixes that carry no meaning on their own.
    while (parts.size() > 1 && (parts.front() == "mob" || parts.front() == "random" || parts.front() == "game"
        || parts.front() == "ambient" || parts.front() == "block" || parts.front() == "entity")) {
        parts.erase(parts.begin());
    }
    std::string text;
    for (const auto part : parts) {
        if (!text.empty()) text += ' ';
        text += part;
    }
    for (auto& character : text) {
        if (character == '_') character = ' ';
        else if (static_cast<unsigned char>(character) < 0x20 || static_cast<unsigned char>(character) > 0x7E) character = '?';
    }
    if (!text.empty()) text[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(text[0])));
    return text;
}

bool SubtitleQueue::Add(const SoundEvent& event, const std::optional<Vec3>& listener) {
    std::string text = event.caption.empty() ? ReadableItemId(event.soundId) : ToUtf8(event.caption);
    if (text.empty() || config_.capacity == 0) return false;
    text = Ellipsize(text, 48);
    if (event.position && !IsPlausibleWorldPosition(*event.position)) return false;
    if (event.position && listener) {
        const double dx = event.position->x - listener->x;
        const double dy = event.position->y - listener->y;
        const double dz = event.position->z - listener->z;
        if (std::sqrt(dx * dx + dy * dy + dz * dz) > config_.maxDistance) return false;
    }
    const std::string key = event.caption.empty() ? event.soundId : text;
    const auto expires = event.occurredAt + config_.lifetime;
    if (auto found = std::ranges::find(captions_, key, &Caption::key); found != captions_.end()) {
        found->position = event.position;
        found->lastHeard = event.occurredAt;
        found->expiresAt = std::max(found->expiresAt, expires);
        found->repeats = std::min(found->repeats + 1, 99);
        return true;
    }
    Caption caption{key, text, event.position, event.occurredAt, expires, 1};
    if (captions_.size() >= config_.capacity) {
        *std::ranges::min_element(captions_, {}, &Caption::expiresAt) = std::move(caption);
    } else {
        captions_.push_back(std::move(caption));
    }
    return true;
}

void SubtitleQueue::Expire(SteadyTime now) {
    std::erase_if(captions_, [&](const Caption& caption) { return caption.expiresAt <= now; });
}

std::wstring StripFormattingCodes(std::wstring_view text) {
    std::wstring result;
    result.reserve(text.size());
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (text[index] == L'§') { ++index; continue; }
        result.push_back(text[index]);
    }
    return result;
}

namespace {
bool IsWordCharacter(wchar_t character) noexcept { return std::iswalnum(character) != 0 || character == L'_'; }
wchar_t Fold(wchar_t character) noexcept { return static_cast<wchar_t>(std::towlower(character)); }
}

std::wstring HideNames(std::wstring_view text, const std::vector<std::wstring>& names, std::wstring_view replacement) {
    std::wstring result;
    std::size_t index = 0;
    while (index < text.size()) {
        bool replaced = false;
        if (index == 0 || !IsWordCharacter(text[index - 1])) {
            for (const auto& name : names) {
                if (name.empty() || name.size() > text.size() - index) continue;
                bool equal = true;
                for (std::size_t offset = 0; offset < name.size() && equal; ++offset) equal = Fold(text[index + offset]) == Fold(name[offset]);
                const std::size_t end = index + name.size();
                if (equal && (end == text.size() || !IsWordCharacter(text[end]))) {
                    result += replacement;
                    index = end;
                    replaced = true;
                    break;
                }
            }
        }
        if (!replaced) result.push_back(text[index++]);
    }
    return result;
}

std::vector<std::wstring> ParseNameList(std::wstring_view list) {
    std::vector<std::wstring> names;
    std::size_t start = 0;
    while (start <= list.size()) {
        const auto comma = list.find(L',', start);
        auto token = list.substr(start, comma == std::wstring_view::npos ? std::wstring_view::npos : comma - start);
        while (!token.empty() && std::iswspace(token.front())) token.remove_prefix(1);
        while (!token.empty() && std::iswspace(token.back())) token.remove_suffix(1);
        if (!token.empty() && names.size() < 16) names.emplace_back(token);
        if (comma == std::wstring_view::npos) break;
        start = comma + 1;
    }
    // Longest first so "Steve_2" is replaced before "Steve".
    std::ranges::sort(names, [](const auto& a, const auto& b) { return a.size() > b.size(); });
    return names;
}

std::wstring PresentChatLine(const ChatEvent& event, const ChatPresentation& options, int hour, int minute) {
    std::wstring sender = event.sender;
    std::wstring message = event.message;
    if (options.stripFormatting) {
        sender = StripFormattingCodes(sender);
        message = StripFormattingCodes(message);
    }
    if (options.hideNicknames && !options.hiddenNames.empty()) {
        sender = HideNames(sender, options.hiddenNames, options.replacement);
        message = HideNames(message, options.hiddenNames, options.replacement);
    }
    std::wstring line;
    if (options.timestamps) line += FromUtf8("[" + FormatClock(hour, minute, 0, options.use24Hour, false) + "] ");
    if (!sender.empty()) line += L"<" + sender + L"> ";
    line += message;
    return line;
}

bool SprintController::WantsSprint(const SprintInputs& inputs) const noexcept {
    if (!inputs.focused || inputs.menuVisible || !inputs.gameplayHasCursor) return false;
    if (!inputs.forwardHeld || inputs.sneakHeld) return false;
    return mode_ == Mode::Always || latched_;
}

std::optional<std::chrono::microseconds> BackgroundFrameInterval(
    const FramePacingConfig& config, bool focused, bool minimized) noexcept {
    if (!config.enabled || focused) return std::nullopt;
    const int fps = std::clamp(minimized ? config.minimizedFps : config.backgroundFps, 1, 120);
    return std::chrono::microseconds(1'000'000 / fps);
}

std::chrono::microseconds PacingDelay(SteadyTime lastPresent, SteadyTime now, std::chrono::microseconds interval) noexcept {
    if (interval <= std::chrono::microseconds::zero() || now < lastPresent) return std::chrono::microseconds::zero();
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - lastPresent);
    if (elapsed >= interval) return std::chrono::microseconds::zero();
    return std::min<std::chrono::microseconds>(interval - elapsed, MaxPacingDelay);
}

namespace {
class SubtitlesModule final : public HudModuleBase<> {
public:
    SubtitlesModule(ModuleMetadata metadata, HudLayout layout, SharedServices&)
        : HudModuleBase(std::move(metadata), {
            FloatSetting("lifetime-seconds", "Caption lifetime", 3.0, 1.0, 10.0, "%.1f s"),
            IntSetting("max-lines", "Maximum captions", 6, 1, 12),
            FloatSetting("max-distance", "Ignore sounds beyond", 16.0, 4.0, 64.0, "%.0f blocks"),
            BoolSetting("show-distance", "Show distance", true)}, layout) {}
    void OnSettingsChanged(const SettingsReader& settings) override {
        std::scoped_lock lock(mutex_);
        SubtitleQueue::Config config;
        config.lifetime = std::chrono::duration_cast<SteadyClock::duration>(std::chrono::duration<double>(settings.Float("lifetime-seconds")));
        config.capacity = static_cast<std::size_t>(settings.Int("max-lines"));
        config.maxDistance = settings.Float("max-distance");
        queue_.SetConfig(config);
        showDistance_ = settings.Bool("show-distance");
    }
    void OnDisable() override { std::scoped_lock lock(mutex_); queue_.Clear(); listener_.reset(); }
    void OnPlayerTransform(const PlayerTransform& transform) override {
        if (!IsPlausibleWorldPosition(transform.position)) return;
        std::scoped_lock lock(mutex_);
        listener_ = transform;
    }
    void OnSound(const SoundEvent& event) override {
        std::scoped_lock lock(mutex_);
        (void)queue_.Add(event, listener_ ? std::optional(listener_->position) : std::nullopt);
    }
    void OnRender(const RenderEvent& event) override {
        std::scoped_lock lock(mutex_);
        NoteRender(event);
        queue_.Expire(event.presentedAt);
    }
    [[nodiscard]] HudContent Content() const override {
        std::scoped_lock lock(mutex_);
        HudContent content;
        for (const auto& caption : queue_.Active()) {
            std::string line = caption.text;
            if (caption.repeats > 1) line += std::format(" x{}", caption.repeats);
            if (caption.position && listener_) {
                const auto direction = listener_->yawDegrees
                    ? DirectionTo(listener_->position, *listener_->yawDegrees, *caption.position) : RelativeDirection::Unknown;
                if (direction != RelativeDirection::Unknown) {
                    line = direction == RelativeDirection::Right ? line + "  >" : std::string(DirectionGlyph(direction)) + "  " + line;
                }
                if (showDistance_) {
                    const double dx = caption.position->x - listener_->position.x;
                    const double dy = caption.position->y - listener_->position.y;
                    const double dz = caption.position->z - listener_->position.z;
                    line += std::format("  {:.0f}m", std::sqrt(dx * dx + dy * dy + dz * dz));
                }
            }
            content.lines.push_back({line, HudTone::Normal});
        }
        return content;
    }

private:
    SubtitleQueue queue_;
    std::optional<PlayerTransform> listener_;
    bool showDistance_{true};
};

class ChatToolsModule final : public HudModuleBase<> {
public:
    static constexpr std::size_t MaxHistory = 50;
    ChatToolsModule(ModuleMetadata metadata, HudLayout layout, SharedServices&)
        : HudModuleBase(std::move(metadata), {
            BoolSetting("timestamps", "Timestamps", true),
            BoolSetting("24-hour", "24-hour timestamps", true),
            BoolSetting("strip-formatting", "Strip color/format codes", true),
            BoolSetting("hide-nicknames", "Hide nicknames", false, "Replaces the names below with the replacement text."),
            TextSetting("hidden-names", "Names to hide (comma separated)", ""),
            TextSetting("replacement", "Replacement", "Player"),
            IntSetting("lines", "Lines shown", 8, 1, 20)}, layout), history_(MaxHistory) {}
    void OnSettingsChanged(const SettingsReader& settings) override {
        std::scoped_lock lock(mutex_);
        options_.timestamps = settings.Bool("timestamps");
        options_.use24Hour = settings.Bool("24-hour");
        options_.stripFormatting = settings.Bool("strip-formatting");
        options_.hideNicknames = settings.Bool("hide-nicknames");
        options_.hiddenNames = ParseNameList(FromUtf8(settings.Text("hidden-names")));
        const auto replacement = settings.Text("replacement");
        options_.replacement = replacement.empty() ? L"Player" : FromUtf8(replacement);
        lines_ = static_cast<std::size_t>(settings.Int("lines"));
    }
    void OnDisable() override { std::scoped_lock lock(mutex_); history_.Clear(); }
    void OnChat(const ChatEvent& event) override {
        std::scoped_lock lock(mutex_);
        // Store the raw event, never anything derived from it on disk or in
        // logs; presentation is applied at draw time so settings changes
        // (e.g. enabling nickname hiding) apply to lines already shown.
        ChatEvent bounded = event;
        if (bounded.message.size() > 256) bounded.message.resize(256);
        if (bounded.sender.size() > 32) bounded.sender.resize(32);
        history_.Push(std::move(bounded));
    }
    [[nodiscard]] HudContent Content() const override {
        std::scoped_lock lock(mutex_);
        HudContent content;
        const std::size_t first = history_.Size() > lines_ ? history_.Size() - lines_ : 0;
        for (std::size_t index = first; index < history_.Size(); ++index) {
            const auto& event = history_.At(index);
            const std::time_t time = std::chrono::system_clock::to_time_t(event.receivedAt);
            std::tm local{};
            const bool known = localtime_s(&local, &time) == 0;
            content.lines.push_back({ToUtf8(PresentChatLine(event, options_, known ? local.tm_hour : 0, known ? local.tm_min : 0)),
                HudTone::Normal});
        }
        return content;
    }

private:
    RingBuffer<ChatEvent> history_;
    ChatPresentation options_;
    std::size_t lines_{8};
};

class ToggleSprintDynamicFpsModule final : public ModuleBase<IModule>, public ISprintAssistModule, public IFramePacingModule {
public:
    ToggleSprintDynamicFpsModule(ModuleMetadata metadata, HudLayout, SharedServices&)
        : ModuleBase(std::move(metadata), {
            BoolSetting("sprint-enabled", "Toggle sprint", true),
            ChoiceSetting("sprint-mode", "Sprint mode", "always", {
                {"always", "Sprint whenever moving forward"}, {"toggle", "Toggle with a key"}}),
            KeySetting("sprint-toggle-key", "Sprint toggle key", 0, "Used in toggle mode."),
            KeySetting("sprint-key", "Game sprint key", VK_CONTROL, "Must match Minecraft's sprint binding."),
            KeySetting("forward-key", "Game forward key", 'W'),
            KeySetting("sneak-key", "Game sneak key", VK_SHIFT),
            BoolSetting("dynamic-fps-enabled", "Dynamic FPS", true),
            IntSetting("background-fps", "Background frame cap", 15, 1, 60, "%d FPS"),
            IntSetting("minimized-fps", "Minimized frame cap", 5, 1, 30, "%d FPS")}) {}
    void OnSettingsChanged(const SettingsReader& settings) override {
        std::scoped_lock lock(mutex_);
        sprintEnabled_ = settings.Bool("sprint-enabled");
        controller_.SetMode(settings.Choice("sprint-mode") == "toggle" ? SprintController::Mode::Toggle : SprintController::Mode::Always);
        toggleKey_ = settings.Key("sprint-toggle-key");
        sprintKey_ = settings.Key("sprint-key");
        forwardKey_ = settings.Key("forward-key");
        sneakKey_ = settings.Key("sneak-key");
        pacing_ = {settings.Bool("dynamic-fps-enabled"), settings.Int("background-fps"), settings.Int("minimized-fps")};
    }
    void OnDisable() override { std::scoped_lock lock(mutex_); controller_.Reset(); }
    void OnKeyboard(const KeyboardEvent& event) override {
        if (event.uiCaptured || !event.pressed || event.repeated) return;
        std::scoped_lock lock(mutex_);
        if (toggleKey_ != 0 && event.key == toggleKey_) controller_.OnToggleKey(true);
    }
    [[nodiscard]] bool WantsSprintHeld(const SprintInputs& inputs) override {
        std::scoped_lock lock(mutex_);
        return sprintEnabled_ && sprintKey_ != 0 && controller_.WantsSprint(inputs);
    }
    [[nodiscard]] std::uint32_t SprintKey() const noexcept override { std::scoped_lock lock(mutex_); return sprintKey_; }
    [[nodiscard]] std::uint32_t ForwardKey() const noexcept override { std::scoped_lock lock(mutex_); return forwardKey_; }
    [[nodiscard]] std::uint32_t SneakKey() const noexcept override { std::scoped_lock lock(mutex_); return sneakKey_; }
    [[nodiscard]] std::optional<std::chrono::microseconds> FrameInterval(bool focused, bool minimized) const override {
        std::scoped_lock lock(mutex_);
        return BackgroundFrameInterval(pacing_, focused, minimized);
    }

private:
    SprintController controller_;
    FramePacingConfig pacing_{};
    bool sprintEnabled_{true};
    std::uint32_t toggleKey_{};
    std::uint32_t sprintKey_{VK_CONTROL};
    std::uint32_t forwardKey_{'W'};
    std::uint32_t sneakKey_{VK_SHIFT};
};

class ToggleSneakModule final : public HudModuleBase<IKeyHoldAssistModule> {
public:
    ToggleSneakModule(ModuleMetadata metadata, HudLayout layout, SharedServices&)
        : HudModuleBase(std::move(metadata), {
            KeySetting("toggle-key", "Toggle key", 'X', "Toggles the synthetic sneak hold."),
            KeySetting("sneak-key", "Game sneak key", VK_SHIFT, "Must match Minecraft's sneak binding.")}, layout) {}
    void OnSettingsChanged(const SettingsReader& settings) override {
        std::scoped_lock lock(mutex_);
        toggleKey_ = settings.Key("toggle-key");
        sneakKey_ = settings.Key("sneak-key");
        latched_ = false;
    }
    void OnDisable() override { ResetHeldKeyState(); }
    void OnKeyboard(const KeyboardEvent& event) override {
        if (event.uiCaptured || !event.pressed || event.repeated) return;
        std::scoped_lock lock(mutex_);
        if (toggleKey_ != 0 && event.key == toggleKey_) latched_ = !latched_;
    }
    [[nodiscard]] std::uint32_t DesiredHeldKey(const HeldKeyInputs& inputs) override {
        std::scoped_lock lock(mutex_);
        if (!inputs.focused || !inputs.gameplayHasCursor || inputs.menuVisible || sneakKey_ == 0) {
            latched_ = false;
            return 0;
        }
        return latched_ ? sneakKey_ : 0;
    }
    void ResetHeldKeyState() noexcept override {
        std::scoped_lock lock(mutex_);
        latched_ = false;
    }
    [[nodiscard]] HudContent Content() const override {
        std::scoped_lock lock(mutex_);
        HudContent content;
        content.bare = true;
        if (latched_) content.lines.push_back({"[Sneaking (Toggled)]", HudTone::Normal});
        return content;
    }

private:
    std::uint32_t toggleKey_{'X'};
    std::uint32_t sneakKey_{VK_SHIFT};
    bool latched_{};
};
}

std::unique_ptr<IModule> MakeSubtitles(ModuleMetadata m, HudLayout l, SharedServices& s) { return std::make_unique<SubtitlesModule>(std::move(m), l, s); }
std::unique_ptr<IModule> MakeChatTools(ModuleMetadata m, HudLayout l, SharedServices& s) { return std::make_unique<ChatToolsModule>(std::move(m), l, s); }
std::unique_ptr<IModule> MakeToggleSprintDynamicFps(ModuleMetadata m, HudLayout l, SharedServices& s) { return std::make_unique<ToggleSprintDynamicFpsModule>(std::move(m), l, s); }
std::unique_ptr<IModule> MakeToggleSneak(ModuleMetadata m, HudLayout l, SharedServices& s) { return std::make_unique<ToggleSneakModule>(std::move(m), l, s); }

}
