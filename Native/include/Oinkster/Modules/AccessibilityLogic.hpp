#pragma once

#include "Oinkster/Events.hpp"
#include "Oinkster/Module.hpp"
#include "Oinkster/Modules/RollingWindow.hpp"
#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Oinkster::Modules {

// ---------------------------------------------------------------- Subtitles

enum class RelativeDirection : std::uint8_t { Unknown, Ahead, Behind, Left, Right };

// Direction of `source` relative to a listener at `listener` facing
// `yawDegrees` (Bedrock convention, 0 = +Z). Horizontal plane only.
[[nodiscard]] RelativeDirection DirectionTo(const Vec3& listener, float yawDegrees, const Vec3& source) noexcept;
[[nodiscard]] std::string_view DirectionGlyph(RelativeDirection direction) noexcept;

// Caption for a Bedrock sound event name ("mob.zombie.say" -> "Zombie say",
// "random.explode" -> "Explode", "step.grass" -> "Step grass"), or empty for
// sounds that are not worth captioning (music, UI clicks).
[[nodiscard]] std::string CaptionForSound(std::string_view soundId);

struct Caption {
    std::string key;
    std::string text;
    std::optional<Vec3> position;
    SteadyTime lastHeard{};
    SteadyTime expiresAt{};
    int repeats{1};
};

// Bounded caption list. A sound whose caption is already showing refreshes
// that caption (position, expiry, repeat count) instead of adding a line;
// when full, the caption closest to expiry is replaced.
class SubtitleQueue {
public:
    struct Config {
        SteadyClock::duration lifetime{std::chrono::seconds(3)};
        std::size_t capacity{6};
        double maxDistance{16.0};
    };
    SubtitleQueue() : SubtitleQueue(Config{}) {}
    explicit SubtitleQueue(Config config) : config_(config) {}
    void SetConfig(Config config) noexcept { config_ = config; }

    // Returns false when the event is ignored (no caption, too far, invalid).
    bool Add(const SoundEvent& event, const std::optional<Vec3>& listener);
    void Expire(SteadyTime now);
    void Clear() noexcept { captions_.clear(); }
    [[nodiscard]] const std::vector<Caption>& Active() const noexcept { return captions_; }

private:
    Config config_;
    std::vector<Caption> captions_;
};

// ---------------------------------------------------------------- Chat tools

// Removes Bedrock formatting codes ("§" followed by one character).
[[nodiscard]] std::wstring StripFormattingCodes(std::wstring_view text);
// Case-insensitive, whole-word replacement of every name in `names`
// (word characters are letters, digits and '_', matching gamertag rules).
[[nodiscard]] std::wstring HideNames(std::wstring_view text, const std::vector<std::wstring>& names, std::wstring_view replacement);
// Splits a comma-separated name list, trimming and dropping empties.
[[nodiscard]] std::vector<std::wstring> ParseNameList(std::wstring_view list);

struct ChatPresentation {
    bool timestamps{true};
    bool use24Hour{true};
    bool stripFormatting{true};
    bool hideNicknames{};
    std::vector<std::wstring> hiddenNames;
    std::wstring replacement{L"Player"};
};

// One presented chat line. Pure: the local time is passed in.
[[nodiscard]] std::wstring PresentChatLine(const ChatEvent& event, const ChatPresentation& options, int hour, int minute);

// ---------------------------------------------------------------- Sprint

// Decides whether the sprint key should be held on the player's behalf.
// It never sprints while a menu/screen owns the cursor, while unfocused,
// while sneaking, or without the player holding forward themselves - i.e.
// it only ever does what holding the sprint key would do.
class SprintController {
public:
    enum class Mode : std::uint8_t { Always, Toggle };
    void SetMode(Mode mode) noexcept { mode_ = mode; if (mode_ == Mode::Always) latched_ = false; }
    void OnToggleKey(bool gameplayActive) noexcept { if (mode_ == Mode::Toggle && gameplayActive) latched_ = !latched_; }
    [[nodiscard]] bool WantsSprint(const SprintInputs& inputs) const noexcept;
    [[nodiscard]] bool Latched() const noexcept { return latched_; }
    void Reset() noexcept { latched_ = false; }

private:
    Mode mode_{Mode::Always};
    bool latched_{};
};

// ---------------------------------------------------------------- Dynamic FPS

struct FramePacingConfig {
    bool enabled{true};
    int backgroundFps{15};
    int minimizedFps{5};
};

// Frame interval to enforce, or nullopt when focused / disabled.
[[nodiscard]] std::optional<std::chrono::microseconds> BackgroundFrameInterval(
    const FramePacingConfig& config, bool focused, bool minimized) noexcept;
// How long to wait before presenting, bounded so the render thread always
// returns promptly (unload waits on it).
inline constexpr std::chrono::milliseconds MaxPacingDelay{250};
[[nodiscard]] std::chrono::microseconds PacingDelay(SteadyTime lastPresent, SteadyTime now,
    std::chrono::microseconds interval) noexcept;

}
