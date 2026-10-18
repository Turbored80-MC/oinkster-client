#include "Oinkster/Modules/Factories.hpp"
#include "Oinkster/Modules/HudFormat.hpp"
#include "Oinkster/Modules/InputLogic.hpp"
#include "Oinkster/Modules/TelemetryLogic.hpp"
#include "Oinkster/Modules/VisualLogic.hpp"
#include "Oinkster/SupportedBuild.hpp"
#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <format>

// Modules matching features other Bedrock clients ship (Blueberry, Flarial)
// that build only on integration points Oinkster already has: the mouse
// queue hook, the window-message path, the presentation adapter's camera
// and the Present hook.
namespace Oinkster::Modules {
namespace {
using std::chrono::milliseconds;
using std::chrono::seconds;

template<typename... Mixins>
class PlainModule : public ModuleBase<IModule>, public Mixins... {
public:
    using ModuleBase<IModule>::ModuleBase;
};

// --- Input --------------------------------------------------------------

class ScrollWheelDisablerModule final : public PlainModule<IMouseShapingModule> {
public:
    ScrollWheelDisablerModule(ModuleMetadata metadata, HudLayout, SharedServices&)
        : PlainModule(std::move(metadata), {
            KeySetting("bypass-key", "Scroll while held", 0,
                "Optional: the wheel reaches the hotbar while this key is held.")}) {}
    void OnSettingsChanged(const SettingsReader& settings) override {
        std::scoped_lock lock(mutex_);
        bypassKey_ = settings.Key("bypass-key");
    }
    void ShapeMouse(MouseShaping& shaping, bool gameplayHasCursor) const override {
        std::scoped_lock lock(mutex_);
        if (!gameplayHasCursor) return;
        if (bypassKey_ != 0 && (GetAsyncKeyState(static_cast<int>(bypassKey_)) & 0x8000) != 0) return;
        shaping.blockWheel = true;
    }

private:
    std::uint32_t bypassKey_{};
};

class SensitivityMultiplierModule final : public PlainModule<IMouseShapingModule> {
public:
    SensitivityMultiplierModule(ModuleMetadata metadata, HudLayout, SharedServices&)
        : PlainModule(std::move(metadata), {
            FloatSetting("horizontal", "Horizontal", 1.0, 0.1, 3.0, "%.2fx"),
            FloatSetting("vertical", "Vertical", 1.0, 0.1, 3.0, "%.2fx"),
            KeySetting("hold-key", "Apply only while held", 0,
                "Optional: e.g. a precision key for bridging. Unset applies always.")}) {}
    void OnSettingsChanged(const SettingsReader& settings) override {
        std::scoped_lock lock(mutex_);
        horizontal_ = settings.Float("horizontal");
        vertical_ = settings.Float("vertical");
        holdKey_ = settings.Key("hold-key");
    }
    void ShapeMouse(MouseShaping& shaping, bool gameplayHasCursor) const override {
        std::scoped_lock lock(mutex_);
        if (!gameplayHasCursor) return;
        if (holdKey_ != 0 && (GetAsyncKeyState(static_cast<int>(holdKey_)) & 0x8000) == 0) return;
        shaping.sensitivityX *= std::clamp(horizontal_, 0.1F, 3.0F);
        shaping.sensitivityY *= std::clamp(vertical_, 0.1F, 3.0F);
    }

private:
    float horizontal_{1.0F};
    float vertical_{1.0F};
    std::uint32_t holdKey_{};
};

class NullMovementModule final : public PlainModule<IKeyTransformModule> {
public:
    NullMovementModule(ModuleMetadata metadata, HudLayout, SharedServices&)
        : PlainModule(std::move(metadata), {
            BoolSetting("horizontal", "Left / right", true),
            BoolSetting("vertical", "Forward / back", false),
            KeySetting("left-key", "Game left key", 'A', "Must match Minecraft's bindings."),
            KeySetting("right-key", "Game right key", 'D'),
            KeySetting("forward-key", "Game forward key", 'W'),
            KeySetting("back-key", "Game back key", 'S')}) {}
    void OnSettingsChanged(const SettingsReader& settings) override {
        std::scoped_lock lock(mutex_);
        const NullMovementFilter::Axis horizontal = settings.Bool("horizontal")
            ? NullMovementFilter::Axis{settings.Key("left-key"), settings.Key("right-key")} : NullMovementFilter::Axis{};
        const NullMovementFilter::Axis vertical = settings.Bool("vertical")
            ? NullMovementFilter::Axis{settings.Key("forward-key"), settings.Key("back-key")} : NullMovementFilter::Axis{};
        filter_.Configure({horizontal, vertical});
    }
    void OnDisable() override { ResetKeyTransform(); }
    [[nodiscard]] bool TransformKey(std::uint32_t key, bool pressed, bool repeated,
        const std::array<bool, 256>& physical, std::vector<KeyMessage>& out) override {
        std::scoped_lock lock(mutex_);
        return filter_.Transform(key, pressed, repeated, physical, out);
    }
    void ResetKeyTransform() noexcept override {
        std::scoped_lock lock(mutex_);
        filter_.Reset();
    }

private:
    NullMovementFilter filter_;
};

class ModernKeyboardHandlingModule final : public PlainModule<IHeldKeyRestoreModule> {
public:
    ModernKeyboardHandlingModule(ModuleMetadata metadata, HudLayout, SharedServices&)
        : PlainModule(std::move(metadata), {
            KeySetting("forward-key", "Game forward key", 'W', "Must match Minecraft's bindings."),
            KeySetting("back-key", "Game back key", 'S'),
            KeySetting("left-key", "Game left key", 'A'),
            KeySetting("right-key", "Game right key", 'D'),
            KeySetting("jump-key", "Game jump key", VK_SPACE),
            KeySetting("sneak-key", "Game sneak key", VK_SHIFT),
            KeySetting("sprint-key", "Game sprint key", VK_CONTROL)}) {}
    void OnSettingsChanged(const SettingsReader& settings) override {
        std::scoped_lock lock(mutex_);
        keys_.clear();
        for (const auto* key : {"forward-key", "back-key", "left-key", "right-key", "jump-key", "sneak-key", "sprint-key"}) {
            if (const auto code = settings.Key(key); code != 0) keys_.push_back(code);
        }
    }
    [[nodiscard]] std::vector<std::uint32_t> KeysToRestore() const override {
        std::scoped_lock lock(mutex_);
        return keys_;
    }

private:
    std::vector<std::uint32_t> keys_{'W', 'S', 'A', 'D', VK_SPACE, VK_SHIFT, VK_CONTROL};
};

// --- Visual -------------------------------------------------------------

class SnapLookModule final : public PlainModule<IPresentationModule> {
public:
    SnapLookModule(ModuleMetadata metadata, HudLayout, SharedServices&)
        : PlainModule(std::move(metadata), {
            ChoiceSetting("view", "Look", "front", {{"front", "Behind you (front view)"}, {"back", "Third person (back view)"}}),
            ChoiceSetting("mode", "Activation", "hold", {{"hold", "Hold"}, {"toggle", "Toggle"}}),
            KeySetting("key", "Activation key", 'V')}) {}
    void OnSettingsChanged(const SettingsReader& settings) override {
        std::scoped_lock lock(mutex_);
        perspective_ = settings.Choice("view") == "back" ? CameraPerspective::ThirdPersonBack : CameraPerspective::ThirdPersonFront;
        toggleMode_ = settings.Choice("mode") == "toggle";
        key_ = settings.Key("key");
        active_ = false;
    }
    void OnDisable() override { std::scoped_lock lock(mutex_); active_ = false; }
    void OnKeyboard(const KeyboardEvent& event) override {
        std::scoped_lock lock(mutex_);
        if (key_ == 0 || event.key != key_) return;
        if (!event.pressed) {
            if (!toggleMode_) active_ = false;
            return;
        }
        if (event.uiCaptured || event.repeated) return;
        active_ = toggleMode_ ? !active_ : true;
    }
    void Contribute(PresentationProfile& profile) const override {
        std::scoped_lock lock(mutex_);
        if (active_) profile.perspective = PerspectiveRequest{perspective_};
    }

private:
    CameraPerspective perspective_{CameraPerspective::ThirdPersonFront};
    std::uint32_t key_{'V'};
    bool toggleMode_{};
    bool active_{};
};

class ChunkBordersModule final : public PlainModule<IPresentationModule, IOverlayModule> {
public:
    ChunkBordersModule(ModuleMetadata metadata, HudLayout, SharedServices&)
        : PlainModule(std::move(metadata), {
            ColorSetting("chunk-color", "Wall grid", 0xFFFFD54AU),
            ColorSetting("corner-color", "Chunk corners", 0xFFFF4A4AU),
            ColorSetting("neighbor-color", "Neighbour corners", 0xFF4AA8FFU),
            FloatSetting("thickness", "Line thickness", 1.5, 0.5, 4.0, "%.1f px"),
            IntSetting("grid", "Grid spacing", 2, 0, 8, "%d blocks", "0 draws the corners only."),
            IntSetting("range", "Vertical range", 24, 4, 96, "%d blocks")}) {}
    void OnSettingsChanged(const SettingsReader& settings) override {
        std::scoped_lock lock(mutex_);
        style_.chunkColor = settings.Color("chunk-color");
        style_.cornerColor = settings.Color("corner-color");
        style_.neighborColor = settings.Color("neighbor-color");
        style_.thickness = settings.Float("thickness");
        style_.gridSpacing = settings.Int("grid");
        style_.verticalRange = settings.Int("range");
    }
    void OnDisable() override { std::scoped_lock lock(mutex_); pose_ = {}; }
    void Contribute(PresentationProfile& profile) const override {
        if (!profile.cameraPose) profile.cameraPose = CameraPoseRequest{};
    }
    void OnCameraPose(const CameraPoseEvent& event) override { std::scoped_lock lock(mutex_); pose_ = event; }
    void OnPlayerTransform(const PlayerTransform& transform) override {
        std::scoped_lock lock(mutex_);
        player_ = transform;
    }
    [[nodiscard]] OverlayPrimitives Primitives(float width, float height) const override {
        std::scoped_lock lock(mutex_);
        if (SteadyClock::now() - pose_.capturedAt > milliseconds(250)) return {};
        // The player's feet place the chunk; the camera alone would move it
        // with a third-person boom.
        std::array<float, 3> at = pose_.position;
        if (SteadyClock::now() - player_.capturedAt < milliseconds(500)) {
            const auto feet = FeetFromEye(player_.position);
            at = {static_cast<float>(feet.x), static_cast<float>(feet.y), static_cast<float>(feet.z)};
        }
        return BuildChunkBorders(style_, ViewOf(pose_), at, width, height);
    }

private:
    ChunkBorderStyle style_{};
    CameraPoseEvent pose_{};
    PlayerTransform player_{};
};

class PostProcessingModule final : public PlainModule<IPostProcessModule> {
public:
    PostProcessingModule(ModuleMetadata metadata, HudLayout, SharedServices&)
        : PlainModule(std::move(metadata), {
            FloatSetting("saturation", "Saturation", 1.2, 0.0, 2.0, "%.2fx"),
            FloatSetting("contrast", "Contrast", 1.05, 0.5, 1.5, "%.2fx"),
            FloatSetting("brightness", "Brightness", 0.0, -0.3, 0.3, "%+.2f"),
            FloatSetting("sharpen", "Sharpen", 0.0, 0.0, 1.0, "%.2f"),
            FloatSetting("vignette", "Vignette", 0.0, 0.0, 1.0, "%.2f"),
            ColorSetting("tint", "Tint", 0xFFFFFFFFU),
            FloatSetting("tint-strength", "Tint strength", 0.0, 0.0, 1.0, "%.2f")}) {}
    void OnSettingsChanged(const SettingsReader& settings) override {
        std::scoped_lock lock(mutex_);
        settings_.saturation = settings.Float("saturation");
        settings_.contrast = settings.Float("contrast");
        settings_.brightness = settings.Float("brightness");
        settings_.sharpen = settings.Float("sharpen");
        settings_.vignette = settings.Float("vignette");
        settings_.tint = settings.Color("tint");
        settings_.tintStrength = settings.Float("tint-strength");
    }
    [[nodiscard]] PostProcessSettings PostProcess() const override { std::scoped_lock lock(mutex_); return settings_; }

private:
    PostProcessSettings settings_{};
};

// --- Telemetry ------------------------------------------------------------

std::string_view PerspectiveName(CameraPerspective perspective) noexcept {
    switch (perspective) {
    case CameraPerspective::FirstPerson: return "first person";
    case CameraPerspective::ThirdPersonBack: return "third person (back)";
    case CameraPerspective::ThirdPersonFront: return "third person (front)";
    case CameraPerspective::Unknown: break;
    }
    return "unknown";
}

std::string_view FaceName(BlockFace face) noexcept {
    switch (face) {
    case BlockFace::Down: return "down";
    case BlockFace::Up: return "up";
    case BlockFace::North: return "north";
    case BlockFace::South: return "south";
    case BlockFace::West: return "west";
    case BlockFace::East: return "east";
    case BlockFace::Unknown: break;
    }
    return "?";
}

std::string_view DimensionName(std::optional<int> dimension) noexcept {
    if (!dimension) return "unknown";
    switch (*dimension) {
    case 0: return "overworld";
    case 1: return "nether";
    case 2: return "the end";
    default: return "other";
    }
}

class DebugMenuModule final : public PlainModule<IDebugOverlayModule, IPresentationModule> {
public:
    DebugMenuModule(ModuleMetadata metadata, HudLayout, SharedServices& services)
        : PlainModule(std::move(metadata), {
            KeySetting("key", "Show key", VK_F3),
            BoolSetting("performance", "Performance", true),
            BoolSetting("network", "Connection", true),
            BoolSetting("target", "Targeted block", true),
            IntSetting("decimals", "Coordinate decimals", 3, 0, 5),
            FloatSetting("scale", "Scale", 1.0, 0.6, 2.0, "%.2fx")}),
          frames_(services.frames), memory_(services.memory) {}
    void OnSettingsChanged(const SettingsReader& settings) override {
        std::scoped_lock lock(mutex_);
        key_ = settings.Key("key");
        performance_ = settings.Bool("performance");
        network_ = settings.Bool("network");
        target_ = settings.Bool("target");
        decimals_ = settings.Int("decimals");
        scale_ = settings.Float("scale");
    }
    void OnDisable() override {
        std::scoped_lock lock(mutex_);
        visible_ = false;
        gameplayVisible_ = false;
        transform_.reset();
        latest_.reset();
        speed_.reset();
        yaw_.reset();
        pitch_.reset();
        ping_.reset();
        pingCaptured_ = {};
        server_.clear();
        selection_ = {};
        perspective_ = CameraPerspective::Unknown;
    }
    void OnKeyboard(const KeyboardEvent& event) override {
        if (!event.pressed || event.repeated || event.uiCaptured) return;
        std::scoped_lock lock(mutex_);
        if (gameplayVisible_ && key_ != 0 && event.key == key_) visible_ = !visible_;
    }
    void OnRender(const RenderEvent& event) override {
        frames_->Record(event);
        std::scoped_lock lock(mutex_);
        NoteRender(event);
        gameplayVisible_ = event.gameplayVisible;
        displayWidth_ = event.width;
        displayHeight_ = event.height;
    }
    void Contribute(PresentationProfile& profile) const override {
        std::scoped_lock lock(mutex_);
        if (!visible_ || !gameplayVisible_ || !target_) return;
        if (!profile.cameraPose) profile.cameraPose = CameraPoseRequest{};
        profile.cameraPose->selection = true;
    }
    void OnPlayerTransform(const PlayerTransform& transform) override {
        std::scoped_lock lock(mutex_);
        if (transform_ && transform.dimension == transform_->dimension) {
            const double seconds = std::chrono::duration<double>(transform.capturedAt - transform_->capturedAt).count();
            if (seconds > 0.05) {
                const double dx = transform.position.x - transform_->position.x;
                const double dz = transform.position.z - transform_->position.z;
                speed_ = std::sqrt(dx * dx + dz * dz) / seconds;
                transform_ = transform;
            }
        } else {
            transform_ = transform;
            speed_.reset();
        }
        if (transform.yawDegrees) yaw_ = transform.yawDegrees;
        if (transform.pitchDegrees) pitch_ = transform.pitchDegrees;
        latest_ = transform;
    }
    void OnNetworkMetrics(const NetworkMetrics& metrics) override {
        std::scoped_lock lock(mutex_);
        ping_ = metrics.pingMilliseconds;
        pingCaptured_ = metrics.capturedAt;
    }
    void OnServerState(const ServerState& state) override {
        std::scoped_lock lock(mutex_);
        if (state.kind == SessionKind::None) {
            server_.clear();
            ping_.reset();
            pingCaptured_ = {};
            transform_.reset();
            latest_.reset();
            speed_.reset();
            yaw_.reset();
            pitch_.reset();
            selection_ = {};
            return;
        }
        server_ = ToUtf8(state.displayName);
    }
    void OnBlockSelection(const BlockSelectionEvent& event) override { std::scoped_lock lock(mutex_); selection_ = event; }
    void OnCameraPerspective(const CameraPerspectiveEvent& event) override { std::scoped_lock lock(mutex_); perspective_ = event.perspective; }

    [[nodiscard]] DebugOverlayContent DebugOverlay() const override {
        std::scoped_lock lock(mutex_);
        DebugOverlayContent content;
        content.visible = visible_ && gameplayVisible_;
        content.scale = scale_;
        if (!content.visible) return content;

        if (performance_) {
            DebugOverlayGroup performance;
            const auto fps = frames_->Fps(std::chrono::seconds(1));
            const auto frame = frames_->FrameMilliseconds(std::chrono::milliseconds(500));
            performance.lines.push_back({std::format("{} fps  T: {} ms",
                fps ? std::format("{:.0f}", *fps) : std::string("--"),
                frame ? std::format("{:.1f}", *frame) : std::string("--")), HudTone::Normal});
            performance.lines.push_back({"Mem: " + FormatBytes(memory_->WorkingSetBytes(RenderNow())), HudTone::Normal});
            content.left.push_back(std::move(performance));
        }

        DebugOverlayGroup world;
        const bool freshTransform = latest_ && (RenderNow() == SteadyTime{} || latest_->capturedAt == SteadyTime{}
            || RenderNow() - latest_->capturedAt <= seconds(1));
        if (freshTransform) {
            const auto feet = FeetFromEye(latest_->position);
            world.lines.push_back({std::format("XYZ: {} / {} / {}", FormatCoordinate(feet.x, false, decimals_),
                FormatCoordinate(feet.y, false, decimals_), FormatCoordinate(feet.z, false, decimals_)), HudTone::Normal});
            const auto bx = static_cast<long long>(std::floor(feet.x));
            const auto by = static_cast<long long>(std::floor(feet.y + 1e-3));
            const auto bz = static_cast<long long>(std::floor(feet.z));
            const auto cx = static_cast<long long>(std::floor(static_cast<double>(bx) / 16.0));
            const auto cz = static_cast<long long>(std::floor(static_cast<double>(bz) / 16.0));
            world.lines.push_back({std::format("Block: {} {} {}", bx, by, bz), HudTone::Normal});
            world.lines.push_back({std::format("Chunk: {} {} in {} {}", bx - cx * 16, bz - cz * 16, cx, cz), HudTone::Normal});
            world.lines.push_back({std::format("Dimension: {}", DimensionName(latest_->dimension)), HudTone::Normal});
        } else {
            world.lines.push_back({"XYZ: --", HudTone::Muted});
        }
        if (freshTransform && yaw_) {
            const float yaw = NormalizeYaw180(*yaw_);
            world.lines.push_back({std::format("Facing: {} ({}) ({:.1f} / {:.1f})", CompassDirection(*yaw_, false), FacingAxis(*yaw_),
                yaw, pitch_.value_or(0.0F)), HudTone::Normal});
        }
        if (freshTransform && speed_) world.lines.push_back({std::format("Speed: {:.2f} blocks/s", *speed_), HudTone::Normal});
        world.lines.push_back({std::format("Camera: {}", PerspectiveName(perspective_)), HudTone::Normal});
        if (target_) {
            if (selection_.hasBlock && selection_.capturedAt != SteadyTime{}
                && RenderNow() - selection_.capturedAt < milliseconds(500)) {
                world.lines.push_back({std::format("Looking at: {} {} {} ({})", selection_.block[0], selection_.block[1], selection_.block[2],
                    FaceName(selection_.face)), HudTone::Normal});
            } else {
                world.lines.push_back({"Looking at: --", HudTone::Muted});
            }
        }
        content.left.push_back(std::move(world));

        DebugOverlayGroup build;
        build.lines.push_back({std::format("Minecraft Bedrock {} ({})", SupportedBuild::GameVersion,
            ToUtf8(SupportedBuild::DisplayVersion)), HudTone::Normal});
        build.lines.push_back({std::format("Oinkster client  protocol {}", SupportedBuild::NetworkProtocol), HudTone::Normal});
        content.right.push_back(std::move(build));
        if (network_) {
            DebugOverlayGroup connection;
            if (!server_.empty()) connection.lines.push_back({"Server: " + server_, HudTone::Normal});
            if (ping_ && (pingCaptured_ == SteadyTime{} || RenderNow() - pingCaptured_ <= seconds(5))) {
                connection.lines.push_back({std::format("Ping: {} ms", *ping_), HudTone::Normal});
            }
            if (!connection.lines.empty()) content.right.push_back(std::move(connection));
        }
        if (displayWidth_ > 0.0F && displayHeight_ > 0.0F) {
            DebugOverlayGroup display;
            display.lines.push_back({std::format("Display: {:.0f}x{:.0f}  DirectX 12", displayWidth_, displayHeight_), HudTone::Normal});
            content.right.push_back(std::move(display));
        }
        return content;
    }

private:
    std::shared_ptr<FrameHistory> frames_;
    std::shared_ptr<ProcessMemorySampler> memory_;
    std::optional<PlayerTransform> transform_;
    std::optional<PlayerTransform> latest_;
    std::optional<double> speed_;
    std::optional<float> yaw_;
    std::optional<float> pitch_;
    std::optional<std::uint32_t> ping_;
    SteadyTime pingCaptured_{};
    std::string server_;
    BlockSelectionEvent selection_{};
    CameraPerspective perspective_{CameraPerspective::Unknown};
    bool performance_{true};
    bool network_{true};
    bool target_{true};
    int decimals_{3};
    std::uint32_t key_{VK_F3};
    float scale_{1.0F};
    float displayWidth_{};
    float displayHeight_{};
    bool visible_{};
    bool gameplayVisible_{};
};
}

std::unique_ptr<IModule> MakeScrollWheelDisabler(ModuleMetadata m, HudLayout l, SharedServices& s) { return std::make_unique<ScrollWheelDisablerModule>(std::move(m), l, s); }
std::unique_ptr<IModule> MakeSensitivityMultiplier(ModuleMetadata m, HudLayout l, SharedServices& s) { return std::make_unique<SensitivityMultiplierModule>(std::move(m), l, s); }
std::unique_ptr<IModule> MakeNullMovement(ModuleMetadata m, HudLayout l, SharedServices& s) { return std::make_unique<NullMovementModule>(std::move(m), l, s); }
std::unique_ptr<IModule> MakeModernKeyboardHandling(ModuleMetadata m, HudLayout l, SharedServices& s) { return std::make_unique<ModernKeyboardHandlingModule>(std::move(m), l, s); }
std::unique_ptr<IModule> MakeSnapLook(ModuleMetadata m, HudLayout l, SharedServices& s) { return std::make_unique<SnapLookModule>(std::move(m), l, s); }
std::unique_ptr<IModule> MakeChunkBorders(ModuleMetadata m, HudLayout l, SharedServices& s) { return std::make_unique<ChunkBordersModule>(std::move(m), l, s); }
std::unique_ptr<IModule> MakePostProcessing(ModuleMetadata m, HudLayout l, SharedServices& s) { return std::make_unique<PostProcessingModule>(std::move(m), l, s); }
std::unique_ptr<IModule> MakeDebugMenu(ModuleMetadata m, HudLayout l, SharedServices& s) { return std::make_unique<DebugMenuModule>(std::move(m), l, s); }

}
