#include "Oinkster/Modules/Factories.hpp"
#include "Oinkster/Modules/VisualLogic.hpp"
#include "Oinkster/ModuleManager.hpp"
#include <algorithm>
#include <cmath>
#include <format>

namespace Oinkster::Modules {

std::uint32_t ApplyOpacity(std::uint32_t argb, float opacity) noexcept {
    const float clamped = std::isfinite(opacity) ? std::clamp(opacity, 0.0F, 1.0F) : 1.0F;
    const auto alpha = static_cast<std::uint32_t>(std::lround(static_cast<float>((argb >> 24U) & 0xFFU) * clamped));
    return (argb & 0x00FFFFFFU) | (alpha << 24U);
}

OverlayPrimitives BuildCrosshair(const CrosshairStyle& style, float width, float height) {
    OverlayPrimitives primitives;
    if (!(width > 0.0F) || !(height > 0.0F)) return primitives;
    const float cx = std::floor(width * 0.5F);
    const float cy = std::floor(height * 0.5F);
    const float thickness = std::clamp(style.thickness, 1.0F, 8.0F);
    const float gap = std::clamp(style.gap, 0.0F, 32.0F);
    const float length = std::clamp(style.length, 0.0F, 48.0F);
    const float half = thickness * 0.5F;
    const auto fill = ApplyOpacity(style.color, style.opacity);
    const auto outlineColor = ApplyOpacity(0xFF000000U, std::clamp(style.opacity, 0.0F, 1.0F) * 0.85F);
    const float border = std::clamp(style.outlineThickness, 0.5F, 3.0F);

    std::vector<OverlayRect> arms;
    if (length > 0.0F) {
        arms.push_back({cx - gap - length, cy - half, cx - gap, cy + half, fill, true, 1.0F});
        arms.push_back({cx + gap, cy - half, cx + gap + length, cy + half, fill, true, 1.0F});
        arms.push_back({cx - half, cy - gap - length, cx + half, cy - gap, fill, true, 1.0F});
        arms.push_back({cx - half, cy + gap, cx + half, cy + gap + length, fill, true, 1.0F});
    }
    if (style.outline) {
        for (const auto& arm : arms) {
            primitives.rects.push_back({arm.x1 - border, arm.y1 - border, arm.x2 + border, arm.y2 + border, outlineColor, true, 1.0F});
        }
    }
    primitives.rects.insert(primitives.rects.end(), arms.begin(), arms.end());
    if (style.centerDot) {
        const float radius = std::clamp(style.dotRadius, 0.5F, 6.0F);
        if (style.outline) primitives.circles.push_back({cx, cy, radius + border, outlineColor, true, 1.0F});
        primitives.circles.push_back({cx, cy, radius, fill, true, 1.0F});
    }
    return primitives;
}

PresentationProfile CollectPresentationProfile(const ModuleManager& modules) {
    PresentationProfile profile;
    for (const auto* module : modules.EnabledModulesOf<IPresentationModule>()) module->Contribute(profile);
    return profile;
}

std::size_t NextFovProfile(std::size_t current, std::size_t count) noexcept {
    return count == 0 ? 0 : (current + 1) % count;
}

namespace {
// Visual-only modules that draw nothing in the HUD.
template<typename... Mixins>
class VisualModule : public ModuleBase<IModule>, public Mixins... {
public:
    using ModuleBase<IModule>::ModuleBase;
};

class CustomCrosshairModule final : public VisualModule<IOverlayModule> {
public:
    CustomCrosshairModule(ModuleMetadata metadata, HudLayout, SharedServices&)
        : VisualModule(std::move(metadata), {
            FloatSetting("size", "Arm length", 6.0, 0.0, 24.0, "%.0f px"),
            FloatSetting("gap", "Gap", 3.0, 0.0, 16.0, "%.0f px"),
            FloatSetting("thickness", "Thickness", 2.0, 1.0, 6.0, "%.1f px"),
            ColorSetting("color", "Color", 0xFFFFFFFFU),
            FloatSetting("opacity", "Opacity", 1.0, 0.1, 1.0, "%.2f"),
            BoolSetting("outline", "Outline", true),
            FloatSetting("outline-thickness", "Outline thickness", 1.0, 0.5, 3.0, "%.1f px"),
            BoolSetting("center-dot", "Center dot", false),
            FloatSetting("dot-size", "Dot radius", 1.5, 0.5, 6.0, "%.1f px")}) {}
    void OnSettingsChanged(const SettingsReader& settings) override {
        CrosshairStyle style;
        style.length = settings.Float("size");
        style.gap = settings.Float("gap");
        style.thickness = settings.Float("thickness");
        style.color = settings.Color("color");
        style.opacity = settings.Float("opacity");
        style.outline = settings.Bool("outline");
        style.outlineThickness = settings.Float("outline-thickness");
        style.centerDot = settings.Bool("center-dot");
        style.dotRadius = settings.Float("dot-size");
        std::scoped_lock lock(mutex_);
        style_ = style;
    }
    [[nodiscard]] OverlayPrimitives Primitives(float width, float height) const override {
        CrosshairStyle style;
        {
            std::scoped_lock lock(mutex_);
            style = style_;
        }
        return BuildCrosshair(style, width, height);
    }

private:
    CrosshairStyle style_;
};

class BlockOutlineModule final : public VisualModule<IPresentationModule> {
public:
    BlockOutlineModule(ModuleMetadata metadata, HudLayout, SharedServices&)
        : VisualModule(std::move(metadata), {
            ColorSetting("color", "Outline color", 0xFFF28FB5U),
            FloatSetting("thickness", "Thickness", 2.0, 1.0, 5.0, "%.1f px"),
            FloatSetting("fill-opacity", "Face fill opacity", 0.0, 0.0, 0.4, "%.2f",
                "Cosmetic tint on the selected face only; never reveals blocks behind others.")}) {}
    void OnSettingsChanged(const SettingsReader& settings) override {
        std::scoped_lock lock(mutex_);
        request_ = {settings.Color("color"), settings.Float("thickness"), settings.Float("fill-opacity")};
    }
    void Contribute(PresentationProfile& profile) const override { std::scoped_lock lock(mutex_); profile.blockOutline = request_; }

private:
    BlockOutlineRequest request_{0xFFF28FB5U, 2.0F, 0.0F};
};

class HurtColorModule final : public VisualModule<IPresentationModule> {
public:
    HurtColorModule(ModuleMetadata metadata, HudLayout, SharedServices&)
        : VisualModule(std::move(metadata), {
            ColorSetting("color", "Tint color", 0xFFFF3030U),
            FloatSetting("opacity", "Tint strength", 0.6, 0.0, 1.0, "%.2f", "0 removes the tint for accessibility.")}) {}
    void OnSettingsChanged(const SettingsReader& settings) override {
        std::scoped_lock lock(mutex_);
        request_ = {settings.Color("color"), settings.Float("opacity")};
    }
    void Contribute(PresentationProfile& profile) const override { std::scoped_lock lock(mutex_); profile.hurtTint = request_; }

private:
    HurtTintRequest request_{0xFFFF3030U, 0.6F};
};

class MotionBlurModule final : public VisualModule<IFrameBlendModule> {
public:
    MotionBlurModule(ModuleMetadata metadata, HudLayout, SharedServices&)
        : VisualModule(std::move(metadata), {
            FloatSetting("strength", "Blend strength", 0.35, 0.05, MaxFrameBlendStrength, "%.2f",
                "Weight of the previous frame blended over the current one.")}) {}
    void OnSettingsChanged(const SettingsReader& settings) override {
        std::scoped_lock lock(mutex_);
        strength_ = std::clamp(settings.Float("strength"), 0.0F, MaxFrameBlendStrength);
    }
    [[nodiscard]] float BlendStrength() const noexcept override { std::scoped_lock lock(mutex_); return strength_; }

private:
    float strength_{0.35F};
};

class FovProfilesModule final : public VisualModule<IPresentationModule> {
public:
    static constexpr std::size_t ProfileCount = 3;
    FovProfilesModule(ModuleMetadata metadata, HudLayout, SharedServices&)
        : VisualModule(std::move(metadata), {
            FloatSetting("profile-1", "Profile 1 FOV", 70.0, MinimumFov, MaximumFov, "%.0f"),
            FloatSetting("profile-2", "Profile 2 FOV", 90.0, MinimumFov, MaximumFov, "%.0f"),
            FloatSetting("profile-3", "Profile 3 FOV", 100.0, MinimumFov, MaximumFov, "%.0f"),
            IntSetting("active", "Active profile", 1, 1, static_cast<int>(ProfileCount)),
            KeySetting("cycle-key", "Cycle profile key", 0, "Switches presets; the range matches the vanilla FOV slider.")}) {}
    void OnSettingsChanged(const SettingsReader& settings) override {
        std::scoped_lock lock(mutex_);
        profiles_ = {settings.Float("profile-1"), settings.Float("profile-2"), settings.Float("profile-3")};
        active_ = static_cast<std::size_t>(settings.Int("active") - 1);
        cycleKey_ = settings.Key("cycle-key");
    }
    void OnKeyboard(const KeyboardEvent& event) override {
        if (event.uiCaptured || !event.pressed || event.repeated) return;
        std::scoped_lock lock(mutex_);
        if (cycleKey_ != 0 && event.key == cycleKey_) active_ = NextFovProfile(active_, ProfileCount);
    }
    [[nodiscard]] std::vector<ModuleAction> Actions() const override { return {{"cycle", "Next profile"}}; }
    bool InvokeAction(std::string_view action, SteadyTime) override {
        if (action != "cycle") return false;
        std::scoped_lock lock(mutex_);
        active_ = NextFovProfile(active_, ProfileCount);
        return true;
    }
    void Contribute(PresentationProfile& profile) const override {
        std::scoped_lock lock(mutex_);
        const auto index = std::min(active_, ProfileCount - 1);
        profile.fov = FovRequest{std::clamp(profiles_[index], MinimumFov, MaximumFov), index};
    }

private:
    std::array<float, ProfileCount> profiles_{70.0F, 90.0F, 100.0F};
    std::size_t active_{};
    std::uint32_t cycleKey_{};
};

class ViewBobbingModule final : public VisualModule<IPresentationModule> {
public:
    ViewBobbingModule(ModuleMetadata metadata, HudLayout, SharedServices&)
        : VisualModule(std::move(metadata), {
            FloatSetting("strength", "Bobbing strength", 0.25, 0.0, 1.0, "%.2f", "0 removes bobbing; 1 is vanilla.")}) {}
    void OnSettingsChanged(const SettingsReader& settings) override {
        std::scoped_lock lock(mutex_);
        request_.strength = std::clamp(settings.Float("strength"), 0.0F, 1.0F);
    }
    void Contribute(PresentationProfile& profile) const override { std::scoped_lock lock(mutex_); profile.viewBobbing = request_; }

private:
    ViewBobbingRequest request_{0.25F};
};

class HandViewModule final : public VisualModule<IPresentationModule> {
public:
    HandViewModule(ModuleMetadata metadata, HudLayout, SharedServices&)
        : VisualModule(std::move(metadata), {
            FloatSetting("offset-x", "Horizontal offset", 0.0, -0.5, 0.5, "%.2f"),
            FloatSetting("offset-y", "Vertical offset", 0.0, -0.5, 0.5, "%.2f"),
            FloatSetting("offset-z", "Depth offset", 0.0, -0.5, 0.5, "%.2f"),
            FloatSetting("scale", "Scale", 1.0, 0.5, 1.5, "%.2fx")}) {}
    void OnSettingsChanged(const SettingsReader& settings) override {
        std::scoped_lock lock(mutex_);
        request_ = {settings.Float("offset-x"), settings.Float("offset-y"), settings.Float("offset-z"), settings.Float("scale")};
    }
    void Contribute(PresentationProfile& profile) const override { std::scoped_lock lock(mutex_); profile.handView = request_; }

private:
    HandViewRequest request_{};
};
}

std::unique_ptr<IModule> MakeCustomCrosshair(ModuleMetadata m, HudLayout l, SharedServices& s) { return std::make_unique<CustomCrosshairModule>(std::move(m), l, s); }
std::unique_ptr<IModule> MakeBlockOutline(ModuleMetadata m, HudLayout l, SharedServices& s) { return std::make_unique<BlockOutlineModule>(std::move(m), l, s); }
std::unique_ptr<IModule> MakeHurtColor(ModuleMetadata m, HudLayout l, SharedServices& s) { return std::make_unique<HurtColorModule>(std::move(m), l, s); }
std::unique_ptr<IModule> MakeMotionBlur(ModuleMetadata m, HudLayout l, SharedServices& s) { return std::make_unique<MotionBlurModule>(std::move(m), l, s); }
std::unique_ptr<IModule> MakeFovProfiles(ModuleMetadata m, HudLayout l, SharedServices& s) { return std::make_unique<FovProfilesModule>(std::move(m), l, s); }
std::unique_ptr<IModule> MakeViewBobbing(ModuleMetadata m, HudLayout l, SharedServices& s) { return std::make_unique<ViewBobbingModule>(std::move(m), l, s); }
std::unique_ptr<IModule> MakeHandView(ModuleMetadata m, HudLayout l, SharedServices& s) { return std::make_unique<HandViewModule>(std::move(m), l, s); }

}
