#pragma once

#include "Oinkster/Module.hpp"
#include "Oinkster/Modules/FrameStatistics.hpp"
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <utility>
#include <vector>

namespace Oinkster::Modules {

// Services shared by several modules. One instance per catalog.
struct SharedServices {
    std::shared_ptr<FrameHistory> frames = std::make_shared<FrameHistory>();
    std::shared_ptr<ProcessMemorySampler> memory = std::make_shared<ProcessMemorySampler>();
    // Wall clock for the Clock module; replaceable in tests.
    std::function<std::chrono::system_clock::time_point()> wallClock = [] { return std::chrono::system_clock::now(); };
};

// Common identity + settings plumbing. Every module guards its own mutable
// state with mutex_, because callbacks arrive from the render thread (render,
// HUD content), the window thread (input), adapter threads (snapshots) and
// the runtime thread (settings, actions).
template<typename Interface = IModule>
class ModuleBase : public Interface {
public:
    ModuleBase(ModuleMetadata metadata, std::vector<SettingDescriptor> settings)
        : metadata_(std::move(metadata)), settings_(std::move(settings)) {}

    [[nodiscard]] const ModuleMetadata& Metadata() const noexcept override { return metadata_; }
    [[nodiscard]] std::span<const SettingDescriptor> Settings() const noexcept override { return settings_; }

protected:
    // The newest presented-frame time seen by OnRender. HUD content is built
    // relative to it (not to SteadyClock::now()) so it is deterministic
    // under test and consistent within a frame.
    void NoteRender(const RenderEvent& event) {
        if (event.presentedAt != SteadyTime{}) renderNow_ = event.presentedAt;
    }
    [[nodiscard]] SteadyTime RenderNow() const { return renderNow_; }

    mutable std::mutex mutex_;

private:
    const ModuleMetadata metadata_;
    const std::vector<SettingDescriptor> settings_;
    SteadyTime renderNow_{};
};

template<typename... Mixins>
class HudModuleBase : public ModuleBase<IHudModule>, public Mixins... {
public:
    HudModuleBase(ModuleMetadata metadata, std::vector<SettingDescriptor> settings, HudLayout defaultLayout)
        : ModuleBase<IHudModule>(std::move(metadata), std::move(settings)),
          defaultLayout_(ClampHudLayout(defaultLayout)), layout_(defaultLayout_) {}

    [[nodiscard]] HudLayout Layout() const noexcept override {
        std::scoped_lock lock(layoutMutex_);
        return layout_;
    }
    void SetLayout(HudLayout value) noexcept override {
        value = ClampHudLayout(value);
        std::scoped_lock lock(layoutMutex_);
        layout_ = value;
    }
    [[nodiscard]] HudLayout DefaultLayout() const noexcept override { return defaultLayout_; }

private:
    mutable std::mutex layoutMutex_;
    const HudLayout defaultLayout_;
    HudLayout layout_;
};

// Small helpers for building descriptors tersely.
[[nodiscard]] inline SettingDescriptor BoolSetting(std::string key, std::string label, bool fallback, std::string help = {}) {
    return {std::move(key), std::move(label), SettingType::Bool, fallback ? "true" : "false", 0, 0, {}, std::move(help), {}};
}
[[nodiscard]] inline SettingDescriptor IntSetting(std::string key, std::string label, int fallback, int minimum, int maximum,
    std::string format = "%d", std::string help = {}) {
    return {std::move(key), std::move(label), SettingType::Int, std::to_string(fallback), static_cast<double>(minimum),
        static_cast<double>(maximum), {}, std::move(help), std::move(format)};
}
[[nodiscard]] inline SettingDescriptor FloatSetting(std::string key, std::string label, double fallback, double minimum, double maximum,
    std::string format = "%.2f", std::string help = {}) {
    return {std::move(key), std::move(label), SettingType::Float, FormatNumberSetting(fallback, SettingType::Float), minimum, maximum,
        {}, std::move(help), std::move(format)};
}
[[nodiscard]] inline SettingDescriptor ColorSetting(std::string key, std::string label, std::uint32_t fallback, std::string help = {}) {
    return {std::move(key), std::move(label), SettingType::Color, FormatColorSetting(fallback), 0, 0, {}, std::move(help), {}};
}
[[nodiscard]] inline SettingDescriptor ChoiceSetting(std::string key, std::string label, std::string fallback,
    std::vector<SettingChoice> choices, std::string help = {}) {
    return {std::move(key), std::move(label), SettingType::Choice, std::move(fallback), 0, 0, std::move(choices), std::move(help), {}};
}
[[nodiscard]] inline SettingDescriptor KeySetting(std::string key, std::string label, std::uint32_t fallback, std::string help = {}) {
    return {std::move(key), std::move(label), SettingType::Key, std::to_string(fallback), 0, 254, {}, std::move(help), {}};
}
[[nodiscard]] inline SettingDescriptor TextSetting(std::string key, std::string label, std::string fallback, std::string help = {}) {
    return {std::move(key), std::move(label), SettingType::Text, std::move(fallback), 0, 0, {}, std::move(help), {}};
}
[[nodiscard]] inline SettingDescriptor PixelSetting(std::string key, std::string label, const PixelGrid& fallback, std::string help = {}) {
    return {std::move(key), std::move(label), SettingType::Pixels, FormatPixelGrid(fallback), 0, 0, {}, std::move(help), {}};
}
// Shows `descriptor` only while the sibling setting `key` holds one of `values`.
[[nodiscard]] inline SettingDescriptor ShownWhen(SettingDescriptor descriptor, std::string key, std::vector<std::string> values) {
    descriptor.visibleWhenKey = std::move(key);
    descriptor.visibleWhenValues = std::move(values);
    return descriptor;
}

// Factory signature used by the catalog table.
using ModuleFactory = std::unique_ptr<IModule> (*)(ModuleMetadata, HudLayout, SharedServices&);

}
