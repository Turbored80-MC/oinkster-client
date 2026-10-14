#include "Oinkster/Modules/Factories.hpp"
#include "Oinkster/Modules/AccessibilityLogic.hpp"
#include "Oinkster/Modules/HudFormat.hpp"
#include "Oinkster/Modules/TelemetryLogic.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <format>

namespace Oinkster::Modules {

float NormalizeYaw360(float yaw) noexcept {
    if (!std::isfinite(yaw)) return 0.0F;
    float value = std::fmod(yaw, 360.0F);
    if (value < 0.0F) value += 360.0F;
    return value >= 360.0F ? 0.0F : value;
}

float NormalizeYaw180(float yaw) noexcept {
    const float value = NormalizeYaw360(yaw);
    return value > 180.0F ? value - 360.0F : value;
}

std::string_view CompassDirection(float yaw, bool intercardinal) noexcept {
    const float value = NormalizeYaw360(yaw);
    if (intercardinal) {
        constexpr std::array<std::string_view, 8> Names{"S", "SW", "W", "NW", "N", "NE", "E", "SE"};
        return Names[static_cast<std::size_t>(std::floor((value + 22.5F) / 45.0F)) % Names.size()];
    }
    constexpr std::array<std::string_view, 4> Names{"South", "West", "North", "East"};
    return Names[static_cast<std::size_t>(std::floor((value + 45.0F) / 90.0F)) % Names.size()];
}

std::string_view FacingAxis(float yaw) noexcept {
    constexpr std::array<std::string_view, 4> Axes{"+Z", "-X", "-Z", "+X"};
    return Axes[static_cast<std::size_t>(std::floor((NormalizeYaw360(yaw) + 45.0F) / 90.0F)) % Axes.size()];
}

bool IsPlausibleWorldPosition(const Vec3& position) noexcept {
    constexpr double Border = 30'000'000.0;
    return std::isfinite(position.x) && std::isfinite(position.y) && std::isfinite(position.z)
        && std::abs(position.x) <= Border && std::abs(position.z) <= Border
        && position.y >= -4096.0 && position.y <= 4096.0;
}

bool IsPlausibleRotation(float pitchDegrees, float yawDegrees) noexcept {
    return std::isfinite(pitchDegrees) && std::isfinite(yawDegrees)
        && pitchDegrees >= -90.0F && pitchDegrees <= 90.0F
        && yawDegrees >= -360.0F && yawDegrees <= 360.0F;
}

Vec3 FeetFromEye(const Vec3& eye) noexcept {
    return {eye.x, eye.y - PlayerEyeHeight, eye.z};
}

std::string FormatCoordinate(double value, bool block, int precision) {
    // The camera is stored as float, so standing on y=50 reads back as
    // 49.9999989 once the eye height is removed; the 1 mm tolerance keeps that
    // from flooring into the block below.
    if (block) return std::format("{}", static_cast<long long>(std::floor(value + 1e-3)));
    const auto text = std::format("{:.{}f}", value, std::clamp(precision, 0, 3));
    // A small negative value rounds to "-0.0"; show it as zero.
    return text.find_first_not_of("-0.") == std::string::npos && text.starts_with('-') ? text.substr(1) : text;
}

namespace {
double Distance(const Vec3& a, const Vec3& b, bool horizontalOnly) {
    const double dx = a.x - b.x;
    const double dy = horizontalOnly ? 0.0 : a.y - b.y;
    const double dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}
}

SpeedEstimator::Result SpeedEstimator::Add(const PlayerTransform& transform) {
    if (!IsPlausibleWorldPosition(transform.position)) return Result::Rejected;
    const Sample sample{transform.capturedAt, transform.position};
    if (samples_.Empty()) {
        samples_.Push(sample);
        dimension_ = transform.dimension;
        return Result::Started;
    }
    const auto& last = samples_.Back();
    if (sample.at <= last.at) return Result::Rejected;
    const auto restart = [&](Result reason) {
        samples_.Clear();
        samples_.Push(sample);
        dimension_ = transform.dimension;
        return reason;
    };
    if (transform.dimension != dimension_) return restart(Result::ResetDimension);
    const auto gap = sample.at - last.at;
    if (gap > config_.maxGap) return restart(Result::ResetGap);
    const double seconds = std::chrono::duration<double>(gap).count();
    if (Distance(sample.position, last.position, false) / seconds > config_.maxPlausibleSpeed) return restart(Result::ResetTeleport);
    samples_.Push(sample);
    while (samples_.Size() > 2 && sample.at - samples_.At(1).at >= config_.window) samples_.PopFront();
    return Result::Accepted;
}

std::optional<double> SpeedEstimator::Speed(bool horizontalOnly) const {
    if (samples_.Size() < 2) return std::nullopt;
    double path{};
    for (std::size_t index = 1; index < samples_.Size(); ++index) {
        path += Distance(samples_.At(index).position, samples_.At(index - 1).position, horizontalOnly);
    }
    const double seconds = std::chrono::duration<double>(samples_.Back().at - samples_.Front().at).count();
    if (seconds <= 0.0) return std::nullopt;
    return path / seconds;
}

std::optional<std::uint32_t> AveragePing(std::span<const std::uint16_t> samples) noexcept {
    std::uint32_t sum{};
    std::uint32_t filled{};
    for (const auto sample : samples) {
        if (sample == RakNetEmptyPing) continue;
        sum += sample;
        ++filled;
    }
    if (filled == 0) return std::nullopt;
    return sum / filled;
}

SessionKind ClassifyRemoteIpv4(const std::array<std::uint8_t, 4>& address) noexcept {
    const auto a = address[0];
    const auto b = address[1];
    if (a == 127) return SessionKind::Singleplayer;
    if (a == 10 || (a == 172 && b >= 16 && b <= 31) || (a == 192 && b == 168) || (a == 169 && b == 254)) {
        return SessionKind::Lan;
    }
    return SessionKind::Server;
}

SessionKind ClassifyRemoteIpv6(const std::array<std::uint8_t, 16>& address) noexcept {
    const bool leadingZeros = std::all_of(address.begin(), address.begin() + 10, [](std::uint8_t byte) { return byte == 0; });
    if (leadingZeros && address[10] == 0xFF && address[11] == 0xFF) {
        return ClassifyRemoteIpv4({address[12], address[13], address[14], address[15]});
    }
    if (leadingZeros && address[10] == 0 && address[11] == 0 && address[12] == 0 && address[13] == 0
        && address[14] == 0 && address[15] == 1) {
        return SessionKind::Singleplayer;
    }
    if ((address[0] & 0xFE) == 0xFC || (address[0] == 0xFE && (address[1] & 0xC0) == 0x80)) return SessionKind::Lan;
    return SessionKind::Server;
}

std::wstring CleanWorldName(std::string_view utf8) {
    std::wstring name = StripFormattingCodes(FromUtf8(utf8));
    const auto isSpace = [](wchar_t character) { return character == L' ' || character == L'\t' || character == L'\r' || character == L'\n'; };
    while (!name.empty() && isSpace(name.back())) name.pop_back();
    std::size_t start = 0;
    while (start < name.size() && isSpace(name[start])) ++start;
    name.erase(0, start);
    if (name.size() > MaxWorldNameLength) name.resize(MaxWorldNameLength);
    return name;
}

void PingTracker::Update(const NetworkMetrics& metrics) noexcept {
    if (!metrics.pingMilliseconds || *metrics.pingMilliseconds > 60'000) return;  // absurd values are misreads
    latest_ = metrics;
}

std::optional<std::uint32_t> PingTracker::Current(SteadyTime now, SteadyClock::duration staleAfter) const noexcept {
    if (!latest_ || now < latest_->capturedAt || now - latest_->capturedAt > staleAfter) return std::nullopt;
    return latest_->pingMilliseconds;
}

namespace {
class PingModule final : public HudModuleBase<> {
public:
    PingModule(ModuleMetadata metadata, HudLayout layout, SharedServices&)
        : HudModuleBase(std::move(metadata), {
            IntSetting("stale-seconds", "Treat as stale after", 5, 1, 30, "%d s"),
            IntSetting("warn-ms", "Highlight above", 150, 20, 1000, "%d ms")}, layout) {}
    void OnSettingsChanged(const SettingsReader& settings) override {
        std::scoped_lock lock(mutex_);
        staleAfter_ = std::chrono::seconds(settings.Int("stale-seconds"));
        warnAt_ = static_cast<std::uint32_t>(settings.Int("warn-ms"));
    }
    void OnDisable() override { std::scoped_lock lock(mutex_); tracker_.Clear(); }
    void OnNetworkMetrics(const NetworkMetrics& metrics) override { std::scoped_lock lock(mutex_); tracker_.Update(metrics); }
    void OnRender(const RenderEvent& event) override { std::scoped_lock lock(mutex_); NoteRender(event); }
    [[nodiscard]] HudContent Content() const override {
        std::scoped_lock lock(mutex_);
        const auto ping = tracker_.Current(RenderNow(), staleAfter_);
        if (!ping) return {{{"Ping  --", HudTone::Muted}}, {}, {}};
        return {{{std::format("Ping  {} ms", *ping), *ping >= warnAt_ ? HudTone::Warning : HudTone::Normal}}, {}, {}};
    }

private:
    PingTracker tracker_;
    SteadyClock::duration staleAfter_{std::chrono::seconds(5)};
    std::uint32_t warnAt_{150};
};

class CoordinatesModule final : public HudModuleBase<> {
public:
    CoordinatesModule(ModuleMetadata metadata, HudLayout layout, SharedServices&)
        : HudModuleBase(std::move(metadata), {
            ChoiceSetting("mode", "Show", "block", {{"block", "Block position (matches the game)"}, {"exact", "Exact position"}}),
            IntSetting("precision", "Decimal places (exact position)", 1, 0, 3),
            ChoiceSetting("layout", "Layout", "single", {{"single", "One line"}, {"stacked", "One axis per line"}}),
            IntSetting("stale-seconds", "Treat as stale after", 2, 1, 30, "%d s")}, layout) {}
    void OnSettingsChanged(const SettingsReader& settings) override {
        std::scoped_lock lock(mutex_);
        block_ = settings.Choice("mode") == "block";
        precision_ = settings.Int("precision");
        stacked_ = settings.Choice("layout") == "stacked";
        staleAfter_ = std::chrono::seconds(settings.Int("stale-seconds"));
    }
    void OnDisable() override { std::scoped_lock lock(mutex_); latest_.reset(); }
    void OnPlayerTransform(const PlayerTransform& transform) override {
        if (!IsPlausibleWorldPosition(transform.position)) return;
        std::scoped_lock lock(mutex_);
        latest_ = transform;
    }
    void OnRender(const RenderEvent& event) override { std::scoped_lock lock(mutex_); NoteRender(event); }
    [[nodiscard]] HudContent Content() const override {
        std::scoped_lock lock(mutex_);
        const bool fresh = latest_ && RenderNow() >= latest_->capturedAt && RenderNow() - latest_->capturedAt <= staleAfter_;
        if (!fresh) return {{{"XYZ  --", HudTone::Muted}}, {}, {}};
        const auto axis = [&](double value) { return FormatCoordinate(value, block_, precision_); };
        const auto& p = latest_->position;
        if (stacked_) return {{{"X  " + axis(p.x), HudTone::Normal}, {"Y  " + axis(p.y), HudTone::Normal}, {"Z  " + axis(p.z), HudTone::Normal}}, {}, {}};
        return {{{"XYZ  " + axis(p.x) + "  " + axis(p.y) + "  " + axis(p.z), HudTone::Normal}}, {}, {}};
    }

private:
    std::optional<PlayerTransform> latest_;
    int precision_{1};
    bool block_{true};
    bool stacked_{};
    SteadyClock::duration staleAfter_{std::chrono::seconds(2)};
};

class DirectionModule final : public HudModuleBase<> {
public:
    DirectionModule(ModuleMetadata metadata, HudLayout layout, SharedServices&)
        : HudModuleBase(std::move(metadata), {
            BoolSetting("intercardinal", "Show intercardinal points (NE, SW ...)", true),
            ChoiceSetting("yaw-range", "Yaw range", "signed", {{"signed", "-180 to 180 (Bedrock)"}, {"unsigned", "0 to 360"}}),
            BoolSetting("show-axis", "Show facing axis", true)}, layout) {}
    void OnSettingsChanged(const SettingsReader& settings) override {
        std::scoped_lock lock(mutex_);
        intercardinal_ = settings.Bool("intercardinal");
        signed_ = settings.Choice("yaw-range") == "signed";
        showAxis_ = settings.Bool("show-axis");
    }
    void OnDisable() override { std::scoped_lock lock(mutex_); yaw_.reset(); }
    void OnPlayerTransform(const PlayerTransform& transform) override {
        if (!transform.yawDegrees || !std::isfinite(*transform.yawDegrees)) return;
        std::scoped_lock lock(mutex_);
        yaw_ = *transform.yawDegrees;
    }
    [[nodiscard]] HudContent Content() const override {
        std::scoped_lock lock(mutex_);
        if (!yaw_) return {{{"Facing  --", HudTone::Muted}}, {}, {}};
        const float shown = signed_ ? NormalizeYaw180(*yaw_) : NormalizeYaw360(*yaw_);
        std::string text = std::format("Facing  {}  {:.1f}", CompassDirection(*yaw_, intercardinal_), shown);
        if (showAxis_) text += std::format("  ({})", FacingAxis(*yaw_));
        return {{{text, HudTone::Normal}}, {}, {}};
    }

private:
    std::optional<float> yaw_;
    bool intercardinal_{true};
    bool signed_{true};
    bool showAxis_{true};
};

class MovementSpeedModule final : public HudModuleBase<> {
public:
    MovementSpeedModule(ModuleMetadata metadata, HudLayout layout, SharedServices&)
        : HudModuleBase(std::move(metadata), {
            ChoiceSetting("unit", "Unit", "bps", {{"bps", "Blocks per second"}, {"kmh", "Kilometres per hour"}, {"mph", "Miles per hour"}}),
            BoolSetting("horizontal", "Horizontal speed only", true),
            IntSetting("window-ms", "Averaging window", 500, 100, 2000, "%d ms")}, layout) {}
    void OnSettingsChanged(const SettingsReader& settings) override {
        std::scoped_lock lock(mutex_);
        unit_ = settings.Choice("unit");
        horizontal_ = settings.Bool("horizontal");
        SpeedEstimator::Config config;
        config.window = std::chrono::milliseconds(settings.Int("window-ms"));
        estimator_ = SpeedEstimator(config);
    }
    void OnDisable() override { std::scoped_lock lock(mutex_); estimator_.Clear(); }
    void OnPlayerTransform(const PlayerTransform& transform) override {
        std::scoped_lock lock(mutex_);
        (void)estimator_.Add(transform);
    }
    [[nodiscard]] HudContent Content() const override {
        std::scoped_lock lock(mutex_);
        const auto speed = estimator_.Speed(horizontal_);
        if (!speed) return {{{"Speed  --", HudTone::Muted}}, {}, {}};
        if (unit_ == "kmh") return {{{std::format("Speed  {:.1f} km/h", *speed * 3.6), HudTone::Normal}}, {}, {}};
        if (unit_ == "mph") return {{{std::format("Speed  {:.1f} mph", *speed * 2.2369363), HudTone::Normal}}, {}, {}};
        return {{{std::format("Speed  {:.2f} b/s", *speed), HudTone::Normal}}, {}, {}};
    }

private:
    SpeedEstimator estimator_;
    std::string unit_{"bps"};
    bool horizontal_{true};
};
}

std::unique_ptr<IModule> MakePing(ModuleMetadata m, HudLayout l, SharedServices& s) { return std::make_unique<PingModule>(std::move(m), l, s); }
std::unique_ptr<IModule> MakeCoordinates(ModuleMetadata m, HudLayout l, SharedServices& s) { return std::make_unique<CoordinatesModule>(std::move(m), l, s); }
std::unique_ptr<IModule> MakeDirection(ModuleMetadata m, HudLayout l, SharedServices& s) { return std::make_unique<DirectionModule>(std::move(m), l, s); }
std::unique_ptr<IModule> MakeMovementSpeed(ModuleMetadata m, HudLayout l, SharedServices& s) { return std::make_unique<MovementSpeedModule>(std::move(m), l, s); }

}
