#include "Oinkster/ModuleCatalog.hpp"
#include "Oinkster/ModuleManager.hpp"
#include "Oinkster/Modules/Factories.hpp"
#include <array>

namespace Oinkster {
namespace {
using enum Capability;
using Modules::ModuleFactory;

struct Definition {
    CatalogEntry entry;
    ModuleFactory factory;
};

const std::vector<Definition>& Definitions() {
    static const std::vector<Definition> definitions{
        {{"fps", L"FPS", L"Current rendered frames per second.", ModuleCategory::Performance, true, {RenderMetrics}}, &Modules::MakeFps},
        {{"frame-time", L"Frame time", L"Smoothed frame pacing in milliseconds.", ModuleCategory::Performance, true, {RenderMetrics}}, &Modules::MakeFrameTime},
        {{"fps-statistics", L"FPS statistics", L"Current, average, minimum, and one-percent-low FPS.", ModuleCategory::Performance, true, {RenderMetrics}}, &Modules::MakeFpsStatistics},
        {{"frame-time-graph", L"Frame-time graph", L"A compact graph of recent frame times.", ModuleCategory::Performance, true, {RenderMetrics}}, &Modules::MakeFrameTimeGraph},
        {{"process-memory", L"Process memory", L"Minecraft's current working-set memory.", ModuleCategory::Performance, true, {RenderMetrics}}, &Modules::MakeProcessMemory},
        {{"performance-summary", L"Performance summary", L"A compact FPS, frame-time, low-FPS, and memory readout.", ModuleCategory::Performance, true, {RenderMetrics}}, &Modules::MakePerformanceSummary},
        {{"cps", L"CPS", L"Local left and right clicks per second.", ModuleCategory::Input, true, {LocalInput}}, &Modules::MakeCps},
        {{"keystrokes", L"Keystrokes", L"Shows local movement, jump, and mouse-button state.", ModuleCategory::Input, true, {LocalInput}}, &Modules::MakeKeystrokes},
        {{"input-latency", L"Input latency", L"Time from an input message to the next rendered frame.", ModuleCategory::Input, true, {LocalInput, RenderMetrics}}, &Modules::MakeInputLatency},
        {{"ping", L"Ping", L"Current connection latency.", ModuleCategory::Telemetry, true, {NetworkMetrics}}, &Modules::MakePing},
        {{"coordinates", L"Coordinates", L"Current player (camera) coordinates.", ModuleCategory::Telemetry, true, {LocalPlayerPosition}}, &Modules::MakeCoordinates},
        {{"direction", L"Direction", L"Facing direction and yaw.", ModuleCategory::Telemetry, true, {LocalPlayerRotation}}, &Modules::MakeDirection},
        {{"movement-speed", L"Movement speed", L"Observed player movement speed.", ModuleCategory::Telemetry, true, {LocalPlayerPosition}}, &Modules::MakeMovementSpeed},
        {{"clock", L"Clock", L"Local system time.", ModuleCategory::Status, true, {}}, &Modules::MakeClock},
        {{"stopwatch", L"Stopwatch", L"A local stopwatch with start, pause, and reset.", ModuleCategory::Status, true, {}}, &Modules::MakeStopwatch},
        {{"session-statistics", L"Session statistics", L"Session duration, input totals, and distance travelled.", ModuleCategory::Status, true, {LocalInput}}, &Modules::MakeSessionStatistics},
        {{"armor-status", L"Armor status", L"Per-slot armor durability.", ModuleCategory::Status, true, {Equipment}}, &Modules::MakeArmorStatus},
        {{"held-item-durability", L"Held item durability", L"Durability of the selected item.", ModuleCategory::Status, true, {Equipment}}, &Modules::MakeHeldItemDurability},
        {{"item-counter", L"Item counter", L"Count of the selected item.", ModuleCategory::Status, true, {Inventory}}, &Modules::MakeItemCounter},
        {{"potion-effects", L"Potion effects", L"Active effect names, levels, and durations.", ModuleCategory::Status, true, {Effects}}, &Modules::MakePotionEffects},
        {{"server-display", L"Server display", L"Current server or world name.", ModuleCategory::Status, true, {ServerState}}, &Modules::MakeServerDisplay},
        {{"resource-pack-display", L"Resource pack display", L"Active resource-pack names.", ModuleCategory::Status, true, {ResourcePacks}}, &Modules::MakeResourcePackDisplay},
        {{"combo-counter", L"Combo counter", L"Confirmed consecutive hits.", ModuleCategory::PvpFeedback, true, {CombatEvents}}, &Modules::MakeComboCounter},
        {{"hit-distance", L"Hit distance", L"Distance of the last confirmed hit.", ModuleCategory::PvpFeedback, true, {CombatEvents}}, &Modules::MakeHitDistance},
        {{"equipment-warnings", L"Equipment warnings", L"Warns about low equipment durability.", ModuleCategory::PvpFeedback, true, {Equipment}}, &Modules::MakeEquipmentWarnings},
        {{"potion-counter", L"Potion counter", L"Counts potions in the visible inventory snapshot.", ModuleCategory::PvpFeedback, true, {Inventory}}, &Modules::MakePotionCounter},
        {{"custom-crosshair", L"Custom crosshair", L"A configurable local crosshair.", ModuleCategory::Visual, false, {FrameComposition}}, &Modules::MakeCustomCrosshair},
        {{"block-outline", L"Block outline", L"Customizes the selected block outline.", ModuleCategory::Visual, false, {SelectedBlockRender}}, &Modules::MakeBlockOutline},
        {{"hurt-color", L"Hurt color", L"Customizes the local damage tint.", ModuleCategory::Visual, false, {DamageRender}}, &Modules::MakeHurtColor},
        {{"motion-blur", L"Motion blur", L"Blends the previous frame for a cosmetic motion blur.", ModuleCategory::Visual, false, {FrameComposition}}, &Modules::MakeMotionBlur},
        {{"fov-profiles", L"FOV profiles", L"Switches between local field-of-view presets.", ModuleCategory::Visual, false, {CameraPresentation}}, &Modules::MakeFovProfiles},
        {{"view-bobbing", L"View bobbing", L"Reduces view bobbing for accessibility.", ModuleCategory::Visual, false, {CameraPresentation}}, &Modules::MakeViewBobbing},
        {{"hand-view", L"Hand view", L"Adjusts local first-person hand presentation.", ModuleCategory::Visual, false, {HandPresentation}}, &Modules::MakeHandView},
        {{"subtitles", L"Subtitles", L"Shows directional sound captions.", ModuleCategory::Accessibility, true, {SoundEvents, LocalPlayerPosition, LocalPlayerRotation}}, &Modules::MakeSubtitles},
        {{"chat-tools", L"Chat tools", L"A local chat view with timestamps, formatting cleanup, and nickname hiding.", ModuleCategory::Accessibility, true, {ChatEvents}}, &Modules::MakeChatTools},
        {{"toggle-sprint-dynamic-fps", L"Toggle sprint and dynamic FPS", L"Holds sprint while moving forward and caps the frame rate in the background.", ModuleCategory::Accessibility, false, {RenderMetrics, LocalInput, InputSynthesis}}, &Modules::MakeToggleSprintDynamicFps},
    };
    return definitions;
}

const std::vector<CatalogEntry>& Entries() {
    static const std::vector<CatalogEntry> entries = [] {
        std::vector<CatalogEntry> result;
        for (const auto& definition : Definitions()) result.push_back(definition.entry);
        return result;
    }();
    return entries;
}
}

std::span<const CatalogEntry> CatalogEntries() { return Entries(); }

std::string_view CategoryLabel(ModuleCategory category) noexcept {
    switch (category) {
    case ModuleCategory::Performance: return "Performance";
    case ModuleCategory::Input: return "Input";
    case ModuleCategory::Status: return "Status";
    case ModuleCategory::Telemetry: return "Telemetry";
    case ModuleCategory::PvpFeedback: return "PvP feedback";
    case ModuleCategory::Visual: return "Visual";
    case ModuleCategory::Accessibility: return "Accessibility";
    }
    return "Other";
}

bool IsStaticallyAvailable(const CatalogEntry& entry) noexcept {
    return std::ranges::all_of(entry.capabilities, [](Capability capability) { return IsCapabilitySupported(capability); });
}

std::wstring StaticUnavailableReason(const CatalogEntry& entry) {
    for (const auto capability : entry.capabilities) {
        if (IsCapabilitySupported(capability)) continue;
        return DefaultCapabilityStatus(capability).reason;
    }
    return {};
}

std::vector<std::unique_ptr<IModule>> CreateModuleCatalog() {
    // One SharedServices per catalog: the performance modules share a single
    // frame history and memory sampler.
    Modules::SharedServices services;
    return CreateModuleCatalog(services);
}

std::vector<std::unique_ptr<IModule>> CreateModuleCatalog(Modules::SharedServices& services) {
    std::vector<std::unique_ptr<IModule>> modules;
    modules.reserve(Definitions().size());
    std::size_t hudIndex{};
    for (const auto& definition : Definitions()) {
        const auto& entry = definition.entry;
        ModuleMetadata metadata{std::string(entry.id), std::wstring(entry.name), std::wstring(entry.description), entry.category,
            false, true, EventPriority::Normal, {}, entry.hud, entry.capabilities};
        HudLayout layout{};
        if (entry.hud) {
            const float column = static_cast<float>(hudIndex % 2);
            const float row = static_cast<float>((hudIndex / 2) % 14);
            layout.normalizedX = 0.02F + column * 0.16F;
            layout.normalizedY = 0.05F + row * 0.065F;
            ++hudIndex;
        }
        modules.push_back(definition.factory(std::move(metadata), layout, services));
    }
    // Modules copy what they need out of `services` (shared_ptrs, the wall
    // clock) at construction and never keep a reference to it.
    return modules;
}

}
