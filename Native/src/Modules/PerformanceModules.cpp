#include "Oinkster/Modules/Factories.hpp"
#include "Oinkster/Modules/HudFormat.hpp"
#include <cmath>
#include <format>

namespace Oinkster::Modules {
namespace {
using std::chrono::milliseconds;
using std::chrono::seconds;

std::string Rounded(std::optional<double> value) {
    return value ? std::format("{:.0f}", *value) : std::string("--");
}

// Base for modules that read the shared frame history. Every one of them
// forwards each RenderEvent; FrameHistory::Record deduplicates by frame
// index, so enabling several never double-counts.
template<typename... Mixins>
class FrameModule : public HudModuleBase<Mixins...> {
public:
    FrameModule(ModuleMetadata metadata, std::vector<SettingDescriptor> settings, HudLayout layout, SharedServices& services)
        : HudModuleBase<Mixins...>(std::move(metadata), std::move(settings), layout), frames_(services.frames) {}
    void OnRender(const RenderEvent& event) override {
        frames_->Record(event);
        std::scoped_lock lock(this->mutex_);
        this->NoteRender(event);
    }

protected:
    std::shared_ptr<FrameHistory> frames_;
};

class FpsModule final : public FrameModule<> {
public:
    FpsModule(ModuleMetadata metadata, HudLayout layout, SharedServices& services)
        : FrameModule(std::move(metadata), {
            IntSetting("smoothing-ms", "Smoothing window", 1000, 250, 3000, "%d ms",
                "FPS is frames / elapsed time over this trailing window."),
            BoolSetting("show-label", "Show label", true)}, layout, services) {}
    void OnSettingsChanged(const SettingsReader& settings) override {
        std::scoped_lock lock(mutex_);
        window_ = milliseconds(settings.Int("smoothing-ms"));
        showLabel_ = settings.Bool("show-label");
    }
    [[nodiscard]] HudContent Content() const override {
        std::scoped_lock lock(mutex_);
        const auto value = Rounded(frames_->Fps(window_));
        return {{{showLabel_ ? "FPS  " + value : value, HudTone::Normal}}, {}, {}};
    }

private:
    milliseconds window_{1000};
    bool showLabel_{true};
};

class FrameTimeModule final : public FrameModule<> {
public:
    FrameTimeModule(ModuleMetadata metadata, HudLayout layout, SharedServices& services)
        : FrameModule(std::move(metadata), {
            IntSetting("smoothing-ms", "Smoothing window", 500, 100, 3000, "%d ms",
                "Mean frame time over this trailing window."),
            IntSetting("decimals", "Decimal places", 1, 0, 2)}, layout, services) {}
    void OnSettingsChanged(const SettingsReader& settings) override {
        std::scoped_lock lock(mutex_);
        window_ = milliseconds(settings.Int("smoothing-ms"));
        decimals_ = settings.Int("decimals");
    }
    [[nodiscard]] HudContent Content() const override {
        std::scoped_lock lock(mutex_);
        const auto value = frames_->FrameMilliseconds(window_);
        std::string text = "Frame  --";
        if (value) {
            text = decimals_ == 0 ? std::format("Frame  {:.0f} ms", *value)
                : decimals_ == 1 ? std::format("Frame  {:.1f} ms", *value) : std::format("Frame  {:.2f} ms", *value);
        }
        return {{{text, HudTone::Normal}}, {}, {}};
    }

private:
    milliseconds window_{500};
    int decimals_{1};
};

class FpsStatisticsModule final : public FrameModule<> {
public:
    FpsStatisticsModule(ModuleMetadata metadata, HudLayout layout, SharedServices& services)
        : FrameModule(std::move(metadata), {
            IntSetting("window-seconds", "Statistics window", 10, 2, 60, "%d s",
                "Average, minimum and 1% low cover this trailing window (bounded to 8192 frames)."),
            BoolSetting("show-frame-count", "Show frame count", false)}, layout, services) {}
    void OnSettingsChanged(const SettingsReader& settings) override {
        std::scoped_lock lock(mutex_);
        window_ = seconds(settings.Int("window-seconds"));
        showCount_ = settings.Bool("show-frame-count");
    }
    [[nodiscard]] HudContent Content() const override {
        std::scoped_lock lock(mutex_);
        const auto stats = frames_->Statistics(window_);
        HudContent content;
        content.lines.push_back({"FPS  " + Rounded(frames_->Fps(milliseconds(500))), HudTone::Normal});
        if (stats.frames == 0) {
            content.lines.push_back({"Avg --  Min --", HudTone::Muted});
            content.lines.push_back({"1% low --", HudTone::Muted});
            return content;
        }
        content.lines.push_back({std::format("Avg {:.0f}  Min {:.0f}", stats.averageFps, stats.minimumFps), HudTone::Normal});
        content.lines.push_back(stats.onePercentLowFps
            ? HudLine{std::format("1% low {:.0f}", *stats.onePercentLowFps), HudTone::Accent}
            : HudLine{std::format("1% low --  ({}/{} frames)", stats.frames, MinimumFramesForOnePercentLow), HudTone::Muted});
        if (showCount_) content.lines.push_back({std::format("{} frames / {} s", stats.frames, window_.count()), HudTone::Muted});
        return content;
    }

private:
    seconds window_{10};
    bool showCount_{};
};

class FrameTimeGraphModule final : public FrameModule<> {
public:
    static constexpr int MaxSamples = 600;
    FrameTimeGraphModule(ModuleMetadata metadata, HudLayout layout, SharedServices& services)
        : FrameModule(std::move(metadata), {
            IntSetting("samples", "History length", 240, 60, MaxSamples, "%d frames"),
            ChoiceSetting("scale", "Vertical scale", "auto", {
                {"auto", "Automatic"}, {"16", "16.7 ms (60 FPS)"}, {"33", "33.3 ms (30 FPS)"},
                {"50", "50 ms"}, {"100", "100 ms"}}),
            BoolSetting("show-current", "Show current value", true)}, layout, services) {}
    void OnSettingsChanged(const SettingsReader& settings) override {
        std::scoped_lock lock(mutex_);
        samples_ = static_cast<std::size_t>(settings.Int("samples"));
        scale_ = settings.Choice("scale");
        showCurrent_ = settings.Bool("show-current");
    }
    [[nodiscard]] HudContent Content() const override {
        std::scoped_lock lock(mutex_);
        HudContent content;
        HudGraph graph;
        graph.values = frames_->RecentFrameMilliseconds(samples_);
        float peak = 0.0F;
        for (const float value : graph.values) peak = std::max(peak, value);
        graph.maximum = scale_ == "16" ? 1000.0F / 60.0F : scale_ == "33" ? 1000.0F / 30.0F
            : scale_ == "50" ? 50.0F : scale_ == "100" ? 100.0F
            : std::max(1000.0F / 60.0F, std::ceil(peak * 1.15F / 5.0F) * 5.0F);
        graph.caption = std::format("{:.0f} ms", graph.maximum);
        if (showCurrent_) {
            content.lines.push_back({graph.values.empty() ? std::string("Frame time  --")
                : std::format("Frame time  {:.1f} ms", graph.values.back()), HudTone::Normal});
        }
        content.graph = std::move(graph);
        return content;
    }

private:
    std::size_t samples_{240};
    std::string scale_{"auto"};
    bool showCurrent_{true};
};

class ProcessMemoryModule final : public HudModuleBase<> {
public:
    ProcessMemoryModule(ModuleMetadata metadata, HudLayout layout, SharedServices& services)
        : HudModuleBase(std::move(metadata), {}, layout), memory_(services.memory) {}
    void OnRender(const RenderEvent& event) override {
        const auto bytes = memory_->WorkingSetBytes(event.presentedAt);
        std::scoped_lock lock(mutex_);
        bytes_ = bytes;
    }
    [[nodiscard]] HudContent Content() const override {
        std::scoped_lock lock(mutex_);
        return {{{"Memory  " + FormatBytes(bytes_), bytes_ ? HudTone::Normal : HudTone::Muted}}, {}, {}};
    }

private:
    std::shared_ptr<ProcessMemorySampler> memory_;
    std::optional<std::uint64_t> bytes_;
};

class PerformanceSummaryModule final : public FrameModule<> {
public:
    PerformanceSummaryModule(ModuleMetadata metadata, HudLayout layout, SharedServices& services)
        : FrameModule(std::move(metadata), {
            IntSetting("window-seconds", "1% low window", 10, 2, 60, "%d s"),
            BoolSetting("show-memory", "Show memory", true)}, layout, services), memory_(services.memory) {}
    void OnSettingsChanged(const SettingsReader& settings) override {
        std::scoped_lock lock(mutex_);
        window_ = seconds(settings.Int("window-seconds"));
        showMemory_ = settings.Bool("show-memory");
    }
    void OnRender(const RenderEvent& event) override {
        FrameModule::OnRender(event);
        const auto bytes = memory_->WorkingSetBytes(event.presentedAt);
        std::scoped_lock lock(mutex_);
        bytes_ = bytes;
    }
    [[nodiscard]] HudContent Content() const override {
        std::scoped_lock lock(mutex_);
        const auto fps = frames_->Fps(seconds(1));
        const auto frame = frames_->FrameMilliseconds(milliseconds(500));
        const auto stats = frames_->Statistics(window_);
        std::string text = std::format("{} FPS  {} ms  1% {}", Rounded(fps),
            frame ? std::format("{:.1f}", *frame) : std::string("--"), Rounded(stats.onePercentLowFps));
        if (showMemory_) text += "  " + FormatBytes(bytes_);
        return {{{text, HudTone::Normal}}, {}, {}};
    }

private:
    std::shared_ptr<ProcessMemorySampler> memory_;
    seconds window_{10};
    bool showMemory_{true};
    std::optional<std::uint64_t> bytes_;
};
}

std::unique_ptr<IModule> MakeFps(ModuleMetadata m, HudLayout l, SharedServices& s) { return std::make_unique<FpsModule>(std::move(m), l, s); }
std::unique_ptr<IModule> MakeFrameTime(ModuleMetadata m, HudLayout l, SharedServices& s) { return std::make_unique<FrameTimeModule>(std::move(m), l, s); }
std::unique_ptr<IModule> MakeFpsStatistics(ModuleMetadata m, HudLayout l, SharedServices& s) { return std::make_unique<FpsStatisticsModule>(std::move(m), l, s); }
std::unique_ptr<IModule> MakeFrameTimeGraph(ModuleMetadata m, HudLayout l, SharedServices& s) { return std::make_unique<FrameTimeGraphModule>(std::move(m), l, s); }
std::unique_ptr<IModule> MakeProcessMemory(ModuleMetadata m, HudLayout l, SharedServices& s) { return std::make_unique<ProcessMemoryModule>(std::move(m), l, s); }
std::unique_ptr<IModule> MakePerformanceSummary(ModuleMetadata m, HudLayout l, SharedServices& s) { return std::make_unique<PerformanceSummaryModule>(std::move(m), l, s); }

}
