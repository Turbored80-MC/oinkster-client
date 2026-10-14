#include "Oinkster/ModuleManager.hpp"
#include "Oinkster/ModuleCatalog.hpp"
#include "Oinkster/SettingsStore.hpp"
#include "Oinkster/SignatureScan.hpp"
#include "Oinkster/SkinHead.hpp"
#include "Oinkster/PlayerSkinLayout.hpp"
#include "Oinkster/ClickGui.hpp"
#include "Oinkster/Dx12Overlay.hpp"
#include "Oinkster/EnttRegistry.hpp"
#include "Oinkster/GameItems.hpp"
#include "Oinkster/InputState.hpp"
#include "Oinkster/Json.hpp"
#include "Oinkster/StatusPipe.hpp"
#include "Oinkster/UnloadGuard.hpp"
#include "Oinkster/WorldTelemetry.hpp"
#include "Oinkster/Modules/AccessibilityLogic.hpp"
#include "Oinkster/Modules/FrameStatistics.hpp"
#include "Oinkster/Modules/HudFormat.hpp"
#include "Oinkster/Modules/InputLogic.hpp"
#include "Oinkster/Modules/PvpLogic.hpp"
#include "Oinkster/Modules/StatusLogic.hpp"
#include "Oinkster/Modules/TelemetryLogic.hpp"
#include "Oinkster/Modules/VisualLogic.hpp"
#include <Windows.h>
#include <array>
#include <atomic>
#include <bitset>
#include <chrono>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <set>
#include <span>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <unordered_set>
#include <vector>

// Deterministic: every time-based test drives explicit timestamps. Nothing
// here sleeps.
namespace {
using namespace std::chrono_literals;
using Oinkster::Capability;
using Oinkster::CapabilityState;
namespace M = Oinkster::Modules;

class ThrowingModule final : public Oinkster::IModule {
public:
    const Oinkster::ModuleMetadata& Metadata() const noexcept override { return metadata_; }
    void OnTick(const Oinkster::TickEvent&) override { throw std::runtime_error("test"); }
private:
    Oinkster::ModuleMetadata metadata_{ "throwing", L"Throwing", L"Test module", Oinkster::ModuleCategory::Telemetry, false };
};

class CancellingModule final : public Oinkster::IModule {
public:
    const Oinkster::ModuleMetadata& Metadata() const noexcept override { return metadata_; }
    void OnKeyboard(const Oinkster::KeyboardEvent& event) override { if (event.key == 'G') event.Cancel(); }
    void OnMouse(const Oinkster::MouseEvent& event) override { if (event.changedButton == 2U) event.Cancel(); }
private:
    Oinkster::ModuleMetadata metadata_{"cancelling", L"Cancelling", L"Test input cancellation",
        Oinkster::ModuleCategory::Accessibility, false, true, Oinkster::EventPriority::Immediate};
};

void Require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
void RequireNear(double actual, double expected, double tolerance, const char* message) {
    if (!(std::abs(actual - expected) <= tolerance)) {
        throw std::runtime_error(std::string(message) + " (expected " + std::to_string(expected) + ", got " + std::to_string(actual) + ")");
    }
}

// A fixed origin well away from the epoch so subtraction never underflows.
const Oinkster::SteadyTime Origin = Oinkster::SteadyTime{} + std::chrono::hours(1);
Oinkster::SteadyTime At(double seconds) {
    return Origin + std::chrono::duration_cast<Oinkster::SteadyClock::duration>(std::chrono::duration<double>(seconds));
}

Oinkster::RenderEvent Frame(std::uint64_t index, double seconds, float delta) {
    Oinkster::RenderEvent event{1920, 1080, 96, delta, false, false, true, index, At(seconds)};
    return event;
}

struct Catalog {
    Oinkster::ModuleManager manager;
    M::SharedServices services;
    Catalog() {
        services.wallClock = [] { return std::chrono::system_clock::time_point{}; };
        services.memory = std::make_shared<M::ProcessMemorySampler>([] { return std::optional<std::uint64_t>(512ULL * 1024 * 1024); });
        for (auto& module : Oinkster::CreateModuleCatalog(services)) manager.Register(std::move(module));
    }
};

std::string ReadFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::stringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

std::string Narrow(std::wstring_view value) { return M::ToUtf8(value); }

// ------------------------------------------------------------------ catalog

void CatalogIdsAreUniqueAndWellFormed() {
    Catalog catalog;
    Require(catalog.manager.Size() == 36, "Catalog must contain 36 modules");
    std::unordered_set<std::string> ids;
    for (const auto& entry : Oinkster::CatalogEntries()) {
        const std::string id(entry.id);
        Require(ids.insert(id).second, "Catalog ids must be unique");
        Require(!id.empty() && std::ranges::all_of(id, [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'; }),
            "Catalog ids must be lowercase kebab-case");
        Require(catalog.manager.FindModule(id) != nullptr, "Every catalog entry must be registered");
        Require((catalog.manager.FindHudModule(id) != nullptr) == entry.hud, "The HUD flag must match the module's interface");
    }
    bool duplicateRejected{};
    try {
        auto duplicate = Oinkster::CreateModuleCatalog();
        catalog.manager.Register(std::move(duplicate.front()));
    } catch (const std::invalid_argument&) { duplicateRejected = true; }
    Require(duplicateRejected, "Duplicate ids must be rejected");
}

void NativeCatalogMatchesLauncherJson() {
    const auto catalogText = ReadFile(OINKSTER_MODULE_CATALOG_JSON);
    Require(!catalogText.empty(), "Assets/module-catalog.json must be readable from the test");
    const auto document = Oinkster::Json::Parse(catalogText);
    Require(document.has_value(), "Assets/module-catalog.json must be valid JSON");
    const auto* schema = document->Find("schemaVersion");
    Require(schema && schema->AsNumber() && *schema->AsNumber() == 1.0, "Catalog schema must be version 1");
    const auto* modules = document->Find("modules");
    Require(modules && modules->AsArray(), "Catalog must contain a modules array");
    const auto entries = Oinkster::CatalogEntries();
    Require(modules->AsArray()->size() == entries.size(), "JSON and native catalogs must list the same number of modules");
    for (std::size_t index = 0; index < entries.size(); ++index) {
        const auto& native = entries[index];
        const auto& json = (*modules->AsArray())[index];
        const auto text = [&](const char* key) { const auto* value = json.Find(key); return value && value->AsString() ? *value->AsString() : std::string("<missing>"); };
        const auto flag = [&](const char* key) { const auto* value = json.Find(key); return value && value->AsBool() && *value->AsBool(); };
        const std::string where = " [" + std::string(native.id) + "]";
        if (text("id") != native.id) throw std::runtime_error("Catalog order/id drift at " + std::to_string(index) + where);
        if (text("name") != Narrow(native.name)) throw std::runtime_error("Catalog name drift" + where);
        if (text("description") != Narrow(native.description)) throw std::runtime_error("Catalog description drift" + where);
        if (text("category") != Oinkster::CategoryLabel(native.category)) throw std::runtime_error("Catalog category drift" + where);
        if (flag("hud") != native.hud) throw std::runtime_error("Catalog HUD flag drift" + where);
        const auto* capabilities = json.Find("capabilities");
        if (!capabilities || !capabilities->AsArray() || capabilities->AsArray()->size() != native.capabilities.size()) {
            throw std::runtime_error("Catalog capability list drift" + where);
        }
        for (std::size_t c = 0; c < native.capabilities.size(); ++c) {
            const auto* id = (*capabilities->AsArray())[c].AsString();
            if (!id || *id != Oinkster::CapabilityId(native.capabilities[c])) throw std::runtime_error("Catalog capability id drift" + where);
        }
        const bool available = Oinkster::IsStaticallyAvailable(native);
        if (flag("available") != available) throw std::runtime_error("Catalog availability drift" + where);
        const auto* reason = json.Find("unavailableReason");
        if (available && reason) throw std::runtime_error("An available module must not carry an unavailableReason" + where);
        if (!available && (!reason || !reason->AsString() || *reason->AsString() != Narrow(Oinkster::StaticUnavailableReason(native)))) {
            throw std::runtime_error("Catalog unavailableReason drift" + where);
        }
    }
}

// ------------------------------------------------------------------ availability

void AvailabilityFollowsCapabilities() {
    Catalog catalog;
    auto& manager = catalog.manager;
    const auto initialRevision = manager.CapabilityRevision();
    for (const auto& entry : Oinkster::CatalogEntries()) {
        const bool liveByDefault = std::ranges::all_of(entry.capabilities, [](Capability capability) {
            return Oinkster::DefaultCapabilityStatus(capability).IsAvailable();
        });
        Require(manager.Availability(entry.id).available == liveByDefault,
            "Runtime availability must follow live capability state, not launcher support");
    }
    Require(manager.SetEnabled("toggle-sprint-dynamic-fps", true), "Toggle sprint / dynamic FPS is deliberately available");
    Require(!manager.SetEnabled("coordinates", true), "Coordinates must wait for validated telemetry");
    const auto reason = manager.Availability("coordinates").unavailableReason;
    Require(reason.find(L"local player position") != std::wstring::npos, "The reason must name the missing capability");

    manager.RestoreEnabled("coordinates", true);
    Require(!manager.IsEnabled("coordinates") && manager.IsRequested("coordinates"),
        "A persisted preference for an unavailable module must be deferred, not dropped");
    manager.SetCapability(Capability::LocalPlayerPosition, {CapabilityState::Available, L"ignored"});
    Require(manager.CapabilityRevision() == initialRevision + 1,
        "A changed capability must advance the status revision");
    manager.SetCapability(Capability::LocalPlayerPosition, {CapabilityState::Available, {}});
    Require(manager.CapabilityRevision() == initialRevision + 1,
        "Reporting the same normalized capability twice must not advance the revision");
    Require(manager.IsEnabled("coordinates"), "A deferred module must activate when its capability validates");
    Require(manager.CapabilityStatusOf(Capability::LocalPlayerPosition).reason.empty(), "Available capabilities carry no reason");
    Require(manager.Availability("movement-speed").available, "Every module needing only that capability becomes available");
    Require(!manager.Availability("subtitles").available, "A module needing several capabilities needs all of them");

    manager.SetCapability(Capability::LocalPlayerPosition, {CapabilityState::Unavailable, L"detached"});
    Require(!manager.IsEnabled("coordinates") && manager.IsRequested("coordinates"),
        "Losing a capability disables the module but keeps the player's preference");
    Require(manager.Availability("coordinates").unavailableReason.find(L"detached") != std::wstring::npos,
        "The adapter's specific reason must reach the module");
    manager.SetCapability(Capability::LocalPlayerPosition, {CapabilityState::Available, {}});
    Require(manager.IsEnabled("coordinates"), "Regaining the capability restores the module");
    Require(manager.SetEnabled("coordinates", false) && !manager.IsRequested("coordinates"),
        "Explicitly disabling clears the preference");

    std::vector<std::string> logged;
    manager.SetDiagnosticsSink([&](std::string_view, std::string_view event, std::string_view message) {
        logged.push_back(std::string(event) + ":" + std::string(message));
    });
    manager.SetCapability(Capability::ChatEvents, {CapabilityState::Unavailable, L"chat signature ambiguous"});
    Require(!logged.empty() && logged.back().find("chat-events=unavailable") != std::string::npos,
        "Capability changes must be logged with the capability id and state");
}

void ModuleLifecycleIsIsolated() {
    Oinkster::ModuleManager manager;
    for (auto& module : Oinkster::CreateModuleCatalog()) manager.Register(std::move(module));
    manager.Register(std::make_unique<ThrowingModule>());
    Require(manager.SetEnabled("throwing", true), "Test module should enable");
    manager.Publish(Oinkster::TickEvent{});
    Require(!manager.IsEnabled("throwing"), "Faulting module must be isolated and disabled");
    manager.Register(std::make_unique<CancellingModule>());
    Require(manager.SetEnabled("cancelling", true), "Input cancellation module should enable");
    Require(!manager.PublishInput(Oinkster::KeyboardEvent{'G', true, false}), "A high-priority module must be able to cancel gameplay keyboard input");
    Require(manager.PublishInput(Oinkster::KeyboardEvent{'H', true, false}), "Uncancelled keyboard input must remain available to gameplay");
    Require(!manager.PublishInput(Oinkster::MouseEvent{0, 0, 0, 2, 2, true}), "A module must be able to cancel a gameplay mouse action");
    Oinkster::ClickGui gui(manager);
    gui.SetSearch(L"frames per second");
    Require(gui.FilteredCatalog().size() == 1, "ClickGUI search must filter modules");
    Require(gui.ToggleModule("fps"), "ClickGUI must toggle available modules");
    Require(manager.HasEnabledHudModules() && manager.HasEnabledOverlayModules(), "An enabled HUD module keeps the overlay render path active");
    Require(!manager.SetEnabled("armor-status", true), "Unavailable modules must reject enable requests");
    manager.DisableAll();
    manager.DisableAll();
    Require(!manager.HasEnabledOverlayModules(), "DisableAll must be idempotent and complete");
    Require(manager.SetEnabled("custom-crosshair", true) && !manager.HasEnabledHudModules() && manager.HasEnabledOverlayModules(),
        "The crosshair alone keeps the overlay render path alive");
    manager.DisableAll();
    Require(manager.SetEnabled("motion-blur", true) && manager.HasEnabledOverlayModules(), "Frame blending keeps the overlay render path alive");
}

void ClickGuiViewsAndFavorites() {
    Catalog catalog;
    auto& manager = catalog.manager;
    Oinkster::ClickGui gui(manager);

    const auto all = gui.FilteredCatalog();
    Require(std::ranges::is_sorted(all, {}, &Oinkster::ModuleMetadata::name),
        "The module browser must use deterministic alphabetical ordering");
    Require(manager.SetEnabled("fps", true), "FPS enables for the smart-view test");
    gui.SetView(Oinkster::ModuleView::Enabled);
    const auto enabled = gui.FilteredCatalog();
    Require(!enabled.empty() && std::ranges::all_of(enabled, [&](const auto& item) { return manager.IsEnabled(item.id); }),
        "The Enabled view contains only active modules");

    gui.SetView(Oinkster::ModuleView::Available);
    Require(std::ranges::all_of(gui.FilteredCatalog(), [](const auto& item) { return item.available; }),
        "The Available view excludes roadmap modules");
    gui.SetView(Oinkster::ModuleView::Roadmap);
    Require(std::ranges::all_of(gui.FilteredCatalog(), [](const auto& item) { return !item.available; }),
        "The Roadmap view contains only unavailable modules");

    Require(gui.ToggleFavorite("fps") && gui.ToggleFavorite("armor-status"), "Known modules can be favorited");
    Require(!gui.ToggleFavorite("missing-module"), "Unknown modules cannot create favorite settings");
    gui.SetView(Oinkster::ModuleView::Favorites);
    Require(gui.FilteredCatalog().size() == 2 && gui.IsFavorite("fps"), "Favorites are exposed as a smart view");
    gui.SetSearch(L"frames per second");
    Require(gui.FilteredCatalog().size() == 1 && gui.FilteredCatalog().front().id == "fps",
        "Search composes with the active smart view");
    gui.SetSearch({});
    gui.SetCategory(Oinkster::ModuleCategory::Performance);
    Require(std::ranges::all_of(gui.FilteredCatalog(), [](const auto& item) {
        return item.category == Oinkster::ModuleCategory::Performance;
    }), "Category views filter independently of smart views");

    const auto captured = Oinkster::CaptureSettings(manager, 0x4B, 0xDC, 0);
    const auto parsed = Oinkster::ParseClientSettings(Oinkster::SerializeClientSettings(captured));
    Require(parsed && parsed->moduleSettings.at("click-gui.favorite.fps") == "true",
        "Favorite keys round-trip through settings JSON");
    Catalog restored;
    Oinkster::ApplySettings(restored.manager, *parsed);
    Oinkster::ClickGui restoredGui(restored.manager);
    Require(restoredGui.IsFavorite("fps") && restoredGui.IsFavorite("armor-status"),
        "Applying saved settings restores favorites");
}

// ------------------------------------------------------------------ settings

void SettingParsingAndNormalization() {
    using Oinkster::SettingType;
    const auto floatSetting = M::FloatSetting("size", "Size", 6.0, 2.0, 24.0);
    Require(Oinkster::NormalizeSetting(floatSetting, "100") == "24", "Floats clamp to the maximum");
    Require(Oinkster::NormalizeSetting(floatSetting, "-5") == "2", "Floats clamp to the minimum");
    Require(Oinkster::NormalizeSetting(floatSetting, "6.000000") == "6", "Legacy fixed-precision floats migrate");
    Require(Oinkster::NormalizeSetting(floatSetting, "nan") == "6" && Oinkster::NormalizeSetting(floatSetting, "7px") == "6",
        "Malformed floats fall back to the default");
    const auto intSetting = M::IntSetting("count", "Count", 5, 1, 10);
    Require(Oinkster::NormalizeSetting(intSetting, "3.6") == "4" && Oinkster::NormalizeSetting(intSetting, "99") == "10", "Ints round and clamp");
    const auto boolSetting = M::BoolSetting("flag", "Flag", true);
    Require(Oinkster::NormalizeSetting(boolSetting, "off") == "false" && Oinkster::NormalizeSetting(boolSetting, "YES") == "true",
        "Bools accept common spellings");
    Require(Oinkster::NormalizeSetting(boolSetting, "maybe") == "true", "Malformed bools use the default");
    const auto colorSetting = M::ColorSetting("color", "Color", 0xFFFFFFFFU);
    Require(Oinkster::NormalizeSetting(colorSetting, "#ffb1cd") == "#FFFFB1CD", "#RRGGBB colors become opaque #AARRGGBB");
    Require(Oinkster::NormalizeSetting(colorSetting, "red") == "#FFFFFFFF", "Malformed colors use the default");
    const auto choice = M::ChoiceSetting("unit", "Unit", "bps", {{"bps", "b/s"}, {"kmh", "km/h"}});
    Require(Oinkster::NormalizeSetting(choice, " KMH ") == "kmh" && Oinkster::NormalizeSetting(choice, "furlongs") == "bps",
        "Choices are case-insensitive and fall back to the default");
    const auto key = M::KeySetting("key", "Key", 0);
    Require(Oinkster::NormalizeSetting(key, "82") == "82", "Ordinary keys are accepted");
    Require(Oinkster::NormalizeSetting(key, "27") == "0" && Oinkster::NormalizeSetting(key, "1") == "0" && Oinkster::NormalizeSetting(key, "91") == "0",
        "Escape, mouse buttons and the Windows key are reserved");
    Require(Oinkster::NormalizeSetting(key, "300") == "0", "Out-of-range keys fall back");
    const auto textSetting = M::TextSetting("names", "Names", "");
    Require(Oinkster::NormalizeSetting(textSetting, "Steve\n\x01" "Alex") == "SteveAlex", "Control characters are stripped from text");
    Require(Oinkster::NormalizeSetting(textSetting, std::string(200, 'a')).size() == Oinkster::MaxTextSettingLength, "Text is bounded");
    const std::string accented = std::string(63, 'a') + "\xC3\xA9";
    Require(Oinkster::NormalizeSetting(textSetting, accented) == std::string(63, 'a'), "Truncation never splits a UTF-8 sequence");
}

void ModuleSettingsRoundTripThroughManager() {
    Catalog catalog;
    auto& manager = catalog.manager;
    Require(manager.SetEnabled("fps", true), "FPS enables");
    manager.SetModuleSetting("custom-crosshair.size", "999");
    Require(manager.ModuleSetting("custom-crosshair.size", "") == "24", "Stored values are normalized against the descriptor");
    manager.SetModuleSetting("click-gui.scale", "0.8");
    Require(manager.ModuleSetting("click-gui.scale", "") == "0.8", "Non-module keys are stored verbatim");
    manager.SetModuleSetting("fps.future-option", "kept");
    Require(manager.ModuleSetting("fps.future-option", "") == "kept", "Unknown keys survive for forward compatibility");

    auto* fps = manager.FindHudModule("fps");
    for (std::uint64_t frame = 1; frame <= 120; ++frame) manager.Publish(Frame(frame, static_cast<double>(frame) / 120.0, 1.0F / 120.0F));
    Require(fps->DisplayText() == "FPS  120", "FPS shows its label by default");
    manager.SetModuleSetting("fps.show-label", "false");
    Require(fps->DisplayText() == "120", "Settings changes reach the module immediately");
    manager.ResetModuleSettings("fps");
    Require(fps->DisplayText() == "FPS  120" && manager.ModuleSetting("fps.show-label", "<none>") == "<none>",
        "Reset to defaults clears stored values and restores behavior");

    manager.ReplaceModuleSettings({{"fps.show-label", "false"}, {"cps.style", "nonsense"}});
    Require(fps->DisplayText() == "120", "A reload applies new values");
    Require(manager.ModuleSetting("cps.style", "") == "labelled", "A reload normalizes malformed values");
    Require(manager.ModuleSetting("click-gui.scale", "<gone>") == "<gone>", "A reload replaces the whole settings map");
}

void SettingsDocumentRoundTripsAndMigrates() {
    Oinkster::ClientSettings settings;
    Require(settings.clickGuiKey == 0x4B, "ClickGUI must default to K");
    settings.moduleStates["fps"] = true;
    settings.keybinds["custom-crosshair"] = 0x43;
    Oinkster::HudLayout layout{};
    layout.normalizedX = 0.25F;
    layout.scale = 1.5F;
    layout.textColor = 0xFF102030U;
    settings.layout["fps"] = layout;
    settings.moduleSettings["custom-crosshair.color"] = "#FFFFB1CD";
    settings.moduleSettings["chat-tools.hidden-names"] = "Steve, \"Alex\"\\";
    const auto parsed = Oinkster::ParseClientSettings(Oinkster::SerializeClientSettings(settings));
    Require(parsed.has_value(), "Serialized settings must parse");
    Require(parsed->moduleStates.at("fps") && parsed->keybinds.at("custom-crosshair") == 0x43, "State and keybinds round-trip");
    RequireNear(parsed->layout.at("fps").normalizedX, 0.25, 1e-5, "HUD position round-trips");
    Require(parsed->layout.at("fps").textColor == 0xFF102030U, "HUD colors round-trip");
    Require(parsed->moduleSettings.at("chat-tools.hidden-names") == "Steve, \"Alex\"\\", "Quotes and backslashes are escaped");
    Require(parsed->moduleSettings.size() == 2, "HUD color strings must not leak into module settings");
    Require(parsed->revision == 1, "Saving advances the revision");

    const auto legacy = Oinkster::ParseClientSettings(
        R"({"schemaVersion":1,"clickGuiKey":77,"modules":{"fps":true},"keybinds":{"bind.fps":70},"layout":{"layout.fps":"0.2,0.3,1.4,0"},"moduleSettings":{"setting.custom-crosshair.size":"9"}})");
    Require(legacy && legacy->clickGuiKey == 77 && legacy->moduleStates.at("fps") && legacy->keybinds.at("fps") == 70,
        "Version-one state, keys and bindings migrate");
    Require(!legacy->layout.at("fps").background && legacy->layout.at("fps").scale == 1.4F, "Version-one layouts migrate");
    Require(legacy->moduleSettings.at("custom-crosshair.size") == "9", "Version-one module settings migrate");

    const auto clamped = Oinkster::ParseClientSettings(
        R"({"schemaVersion":2,"menuKey":5000,"unloadKey":-1,"modules":{"fps":{"enabled":true,"keybind":"x","hud":{"x":7,"scale":"big","backgroundColor":"#zz"}},"Bad Id":{"enabled":true}}})");
    Require(clamped && clamped->clickGuiKey == 0x4B && clamped->unloadKey == 0xDC, "Invalid control keys fall back to defaults");
    Require(clamped->layout.at("fps").normalizedX == 1.0F && clamped->layout.at("fps").scale == 1.0F
        && clamped->layout.at("fps").backgroundColor == 0xFF111417U, "Malformed HUD values clamp or fall back");
    Require(!clamped->keybinds.contains("fps") && !clamped->moduleStates.contains("Bad Id"), "Malformed bindings and ids are dropped");
    Require(!Oinkster::ParseClientSettings("{\"schemaVersion\":3}").has_value(), "Unknown schemas are rejected");
    Require(!Oinkster::ParseClientSettings("not json").has_value(), "Corrupt files are rejected");

    const auto directory = std::filesystem::current_path() / "test-artifacts" / "OinksterSettings";
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
    std::filesystem::create_directories(directory);
    Oinkster::SettingsStore store(directory / "settings.json");
    store.Save(settings);
    Require(store.Load().moduleStates.at("fps"), "Settings round-trip through disk");
    { std::ofstream corrupt(directory / "settings.json", std::ios::trunc); corrupt << "{oops"; }
    Require(store.Load().moduleStates.empty(), "A corrupt file loads defaults");
    bool quarantined{};
    for (const auto& entry : std::filesystem::directory_iterator(directory)) quarantined |= entry.path().filename().wstring().contains(L".corrupt.");
    Require(quarantined && !std::filesystem::exists(directory / "settings.json"), "A corrupt file is quarantined");
    std::filesystem::remove_all(directory, ignored);
}

void SettingsApplyAndCapturePreserveDeferredModules() {
    Catalog source;
    Require(source.manager.SetEnabled("fps", true), "FPS enables");
    source.manager.RestoreEnabled("armor-status", true);
    (void)source.manager.SetKeybind("cps", 'C', 0x4B, 0xDC);
    source.manager.SetModuleSetting("clock.24-hour", "true");
    auto* fps = source.manager.FindHudModule("fps");
    auto layout = fps->Layout();
    layout.normalizedY = 0.5F;
    fps->SetLayout(layout);
    const auto captured = Oinkster::CaptureSettings(source.manager, 0x4B, 0xDC, 4);
    Require(captured.moduleStates.at("armor-status"), "A deferred module is saved as enabled");
    Require(!captured.moduleStates.at("clock"), "Untouched modules are saved as disabled");

    Catalog target;
    Oinkster::ApplySettings(target.manager, Oinkster::ParseClientSettings(Oinkster::SerializeClientSettings(captured)).value());
    Require(target.manager.IsEnabled("fps") && target.manager.IsRequested("armor-status") && !target.manager.IsEnabled("armor-status"),
        "Applying restores active and deferred modules");
    Require(target.manager.Keybind("cps") == 'C', "Applying restores keybinds");
    Require(target.manager.ModuleSetting("clock.24-hour", "") == "true", "Applying restores module settings");
    RequireNear(target.manager.FindHudModule("fps")->Layout().normalizedY, 0.5, 1e-5, "Applying restores HUD layouts");
}

void KeybindConflictsAndReservedKeys() {
    Catalog catalog;
    auto& manager = catalog.manager;
    Require(manager.SetKeybind("fps", 'C', 'K', 0xDC), "A normal module keybind must be accepted");
    Require(manager.SetKeybind("cps", 'C', 'K', 0xDC), "Duplicate module keybinds are allowed (group toggles)");
    Require(manager.KeybindConflicts("fps") == std::vector<std::string>{"cps"}, "Conflicts must be reported for the UI");
    Require(!manager.SetKeybind("clock", 'K', 'K', 0xDC), "The menu key must remain reserved");
    Require(!manager.SetKeybind("clock", 0xDC, 'K', 0xDC), "The unload key must remain reserved");
    Require(!manager.SetKeybind("clock", VK_ESCAPE, 'K', 0xDC) && !manager.SetKeybind("clock", VK_LBUTTON, 'K', 0xDC)
        && !manager.SetKeybind("clock", VK_LWIN, 'K', 0xDC), "Escape, mouse buttons and Windows keys are reserved");
    Require(!manager.SetKeybind("no-such-module", 'J', 'K', 0xDC), "Unknown modules cannot be bound");
    const bool fpsBefore = manager.IsEnabled("fps"), cpsBefore = manager.IsEnabled("cps");
    Require(manager.HandleKeybind('C'), "A bound key must report that it toggled something");
    Require(manager.IsEnabled("fps") != fpsBefore && manager.IsEnabled("cps") != cpsBefore, "A duplicate binding toggles every bound module");
    Require(!manager.HandleKeybind('Q'), "An unbound key must report no change (no save loop)");
    Require(manager.SetKeybind("armor-status", 'U', 'K', 0xDC) && !manager.HandleKeybind('U'),
        "A key bound to an unavailable module changes nothing and must not trigger a save");
    Require(manager.SetKeybind("fps", 0, 'K', 0xDC) && manager.Keybind("fps") == 0, "Binding 0 clears");
}

// ------------------------------------------------------------------ performance

void RollingFpsAndFrameTime() {
    M::FrameHistory history;
    for (std::uint64_t frame = 1; frame <= 240; ++frame) {
        Require(history.Record(Frame(frame, static_cast<double>(frame) / 120.0, 1.0F / 120.0F)), "Frames record");
    }
    Require(!history.Record(Frame(240, 2.0, 1.0F / 120.0F)), "A repeated frame index is ignored");
    Require(!history.Record(Frame(10, 2.1, 1.0F / 120.0F)), "An out-of-order frame index is ignored");
    RequireNear(*history.Fps(1s), 120.0, 0.5, "FPS is frames over elapsed time");
    RequireNear(*history.FrameMilliseconds(500ms), 1000.0 / 120.0, 0.01, "Frame time is the windowed mean");
    for (std::uint64_t frame = 241; frame <= 300; ++frame) (void)history.Record(Frame(frame, 2.0 + static_cast<double>(frame - 240) / 30.0, 1.0F / 30.0F));
    RequireNear(*history.Fps(1s), 30.0, 0.5, "A sustained change shows up within the smoothing window");
    Require(!M::FrameHistory().Fps(1s).has_value(), "No frames means no FPS, not zero");
}

void OnePercentLowIsTheSlowestPercentNotTheMinimum() {
    std::vector<double> frames(99, 0.010);
    Require(!M::ComputeFrameStatistics(frames).onePercentLowFps, "Fewer than 100 frames report no 1% low");
    frames.assign(198, 0.010);
    frames.push_back(0.050);
    frames.push_back(0.030);
    const auto stats = M::ComputeFrameStatistics(frames);
    // Slowest 1% of 200 frames = 2 frames: (50 + 30) / 2 = 40 ms -> 25 FPS.
    RequireNear(*stats.onePercentLowFps, 25.0, 1e-6, "1% low is the mean of the slowest 1% of frames");
    RequireNear(stats.minimumFps, 20.0, 1e-6, "Minimum FPS is the single slowest frame");
    RequireNear(stats.averageFps, 200.0 / (198 * 0.010 + 0.080), 1e-6, "Average FPS is time-weighted");
    Require(*stats.onePercentLowFps != stats.minimumFps, "1% low must not be the lowest single sample");
    frames.assign(100, 0.010);
    frames[37] = 0.050;
    RequireNear(*M::ComputeFrameStatistics(frames).onePercentLowFps, 20.0, 1e-6, "With 100 frames the slowest frame is the 1%");
}

void GraphHistoryIsBounded() {
    M::FrameHistory history(512);
    for (std::uint64_t frame = 1; frame <= 5000; ++frame) (void)history.Record(Frame(frame, static_cast<double>(frame) / 144.0, 1.0F / 144.0F));
    Require(history.Size() == 512, "Frame history is capped at its capacity");
    const auto recent = history.RecentFrameMilliseconds(240);
    Require(recent.size() == 240, "Graph samples are bounded by the requested window");
    Catalog catalog;
    Require(catalog.manager.SetEnabled("frame-time-graph", true), "Graph enables");
    catalog.manager.SetModuleSetting("frame-time-graph.samples", "100000");
    for (std::uint64_t frame = 1; frame <= 1000; ++frame) catalog.manager.Publish(Frame(frame, static_cast<double>(frame) / 60.0, 1.0F / 60.0F));
    const auto content = catalog.manager.FindHudModule("frame-time-graph")->Content();
    Require(content.graph && content.graph->values.size() == 600, "The graph's sample count is clamped to its maximum");
    // 16.7 ms peak + 15% headroom, rounded up to 5 ms.
    RequireNear(content.graph->maximum, 20.0, 0.01, "Automatic scale fits the data with headroom");
}

void PerformanceModulesShareOneFrameHistory() {
    Catalog catalog;
    auto& manager = catalog.manager;
    for (const auto* id : {"fps", "frame-time", "fps-statistics", "performance-summary", "process-memory"}) Require(manager.SetEnabled(id, true), id);
    for (std::uint64_t frame = 1; frame <= 300; ++frame) manager.Publish(Frame(frame, static_cast<double>(frame) / 100.0, 0.010F));
    Require(catalog.services.frames->Size() == 300, "Several enabled performance modules must not double-count frames");
    Require(manager.FindHudModule("fps-statistics")->DisplayText().contains("1% low 100"), "Statistics publish a real 1% low");
    Require(manager.FindHudModule("process-memory")->DisplayText() == "Memory  512 MB", "Process memory formats the sampled working set");
    Require(manager.FindHudModule("performance-summary")->DisplayText() == "100 FPS  10.0 ms  1% 100  512 MB", "The summary combines shared metrics");
    int queries{};
    M::ProcessMemorySampler sampler([&] { ++queries; return std::optional<std::uint64_t>(); }, 500ms);
    (void)sampler.WorkingSetBytes(At(0));
    (void)sampler.WorkingSetBytes(At(0.2));
    (void)sampler.WorkingSetBytes(At(0.6));
    Require(queries == 2, "Memory is queried at most once per refresh interval, never per frame");
    Require(M::FormatBytes(std::nullopt) == "--", "Failed queries format safely");
}

// ------------------------------------------------------------------ input

void CpsUsesIndependentRollingWindows() {
    M::EventRateWindow window(8);
    window.Add(At(0.0));
    window.Add(At(0.5));
    window.Add(At(0.99));
    Require(window.Count(At(0.99), 1s) == 3, "Clicks within the last second count");
    Require(window.Count(At(1.2), 1s) == 2, "Clicks older than one second expire");
    for (int i = 0; i < 100; ++i) window.Add(At(2.0));
    Require(window.Count(At(2.0), 1s) == 8, "The window is bounded");

    Catalog catalog;
    Require(catalog.manager.SetEnabled("cps", true), "CPS enables");
    auto click = [&](std::uint32_t button, double seconds, bool captured) {
        Oinkster::MouseEvent event{0, 0, 0, button, button, true, captured, At(seconds)};
        catalog.manager.Publish(event);
    };
    click(1, 0.1, false);
    click(1, 0.2, false);
    click(2, 0.3, false);
    click(1, 0.4, true);
    catalog.manager.Publish(Frame(1, 0.5, 0.016F));
    Require(catalog.manager.FindHudModule("cps")->DisplayText() == "CPS  L 2  R 1", "Left and right are independent; UI-captured clicks are ignored");
    catalog.manager.Publish(Frame(2, 1.35, 0.016F));
    Require(catalog.manager.FindHudModule("cps")->DisplayText() == "CPS  L 0  R 0", "Counts decay after one second");
}

void KeystrokesTrackOnlyGameInput() {
    Catalog catalog;
    Require(catalog.manager.SetEnabled("keystrokes", true), "Keystrokes enable");
    auto* module = catalog.manager.FindHudModule("keystrokes");
    catalog.manager.Publish(Oinkster::KeyboardEvent{'W', true, false});
    Require(module->DisplayText().contains("[W]"), "Keystrokes reflect a pressed movement key");
    catalog.manager.Publish(Oinkster::KeyboardEvent{'W', false, false});
    Require(!module->DisplayText().contains("[W]"), "Keystrokes release a movement key");
    catalog.manager.Publish(Oinkster::KeyboardEvent{'A', true, false, true});
    Require(!module->DisplayText().contains("[A]"), "Presses the ClickGUI captured are not shown");
    catalog.manager.Publish(Oinkster::KeyboardEvent{VK_LSHIFT, true, false});
    catalog.manager.SetModuleSetting("keystrokes.show-sneak-sprint", "true");
    Require(module->DisplayText().contains("[Shift]"), "Optional keys are configurable and left/right variants normalize");
    catalog.manager.Publish(Oinkster::MouseEvent{0, 0, 0, 1, 1, true});
    Require(module->DisplayText().contains("[LMB]"), "Mouse buttons are shown");
    Oinkster::RenderEvent unfocused = Frame(1, 1.0, 0.016F);
    unfocused.gameFocused = false;
    catalog.manager.Publish(unfocused);
    Require(!module->DisplayText().contains("["), "Losing focus clears every key so input to other apps is never shown");
    catalog.manager.SetModuleSetting("keystrokes.forward-key", std::to_string('Z'));
    Require(module->DisplayText().contains(" Z "), "Movement keys are rebindable");
}

void KeystrokesShowsMouseCps() {
    Catalog catalog;
    Require(catalog.manager.SetEnabled("keystrokes", true), "Keystrokes enable");
    auto* module = catalog.manager.FindHudModule("keystrokes");
    auto click = [&](std::uint32_t button, double seconds, bool pressed, bool captured = false) {
        catalog.manager.Publish(Oinkster::MouseEvent{0, 0, 0, pressed ? button : 0U, button, pressed, captured, At(seconds)});
    };
    click(1, 0.1, true);
    click(1, 0.15, false);
    click(1, 0.2, true);
    click(1, 0.25, false);
    click(2, 0.3, true);
    click(2, 0.35, false);
    click(1, 0.4, true, true);
    catalog.manager.Publish(Frame(1, 0.5, 0.016F));
    const auto content = module->Content();
    Require(content.bare, "Keystrokes draw bare tiles without a HUD panel");
    Require(content.keyRows.size() >= 3 && content.keyRows[0].size() == 3 && content.keyRows[0][0].spacer
            && content.keyRows[0][2].spacer && content.keyRows[0][1].label == "W",
        "W sits centered between spacers above A S D");
    for (const auto& row : content.keyRows) {
        float span{};
        for (const auto& key : row) span += key.widthUnits;
        Require(span == 3.0F, "Every keystrokes row spans the same three columns");
    }
    Require(module->DisplayText().contains("LMB (2 CPS)") && module->DisplayText().contains("RMB (1 CPS)"),
        "Mouse keys show clicks per second; UI-captured clicks are ignored");
    catalog.manager.Publish(Frame(2, 1.6, 0.016F));
    Require(module->DisplayText().contains("LMB (0 CPS)"), "Mouse CPS decays after one second");
    catalog.manager.SetModuleSetting("keystrokes.show-cps", "false");
    Require(!module->DisplayText().contains("CPS"), "The CPS caption can be turned off");
}

void KeystrokesFollowPhysicalKeyState() {
    Catalog catalog;
    Require(catalog.manager.SetEnabled("keystrokes", true), "Keystrokes enable");
    auto* module = catalog.manager.FindHudModule("keystrokes");
    auto frame = [&](std::uint64_t index, double seconds, std::initializer_list<int> held, bool menu = false) {
        auto event = Frame(index, seconds, 0.016F);
        event.menuVisible = menu;
        std::bitset<256> keys;
        for (const int key : held) keys[static_cast<std::size_t>(key)] = true;
        event.keysDown = keys;
        catalog.manager.Publish(event);
    };
    frame(1, 0.10, {'W', VK_LBUTTON});
    Require(module->DisplayText().contains("[W]") && module->DisplayText().contains("[LMB"),
        "Held keys show without any window message (the game reads raw input)");
    frame(2, 0.12, {});
    frame(3, 0.14, {VK_LBUTTON});
    frame(4, 0.16, {});
    Require(!module->DisplayText().contains("[W]") && module->DisplayText().contains("LMB (2 CPS)"),
        "Releases clear keys and physical press edges count as clicks");
    catalog.manager.Publish(Oinkster::MouseEvent{0, 0, 0, 1, 1, true, false, At(0.20)});
    frame(5, 0.21, {VK_LBUTTON});
    catalog.manager.Publish(Oinkster::MouseEvent{0, 0, 0, 0, 1, false, false, At(0.22)});
    frame(6, 0.23, {});
    frame(7, 0.25, {VK_LBUTTON});
    Require(module->DisplayText().contains("[LMB](3 CPS)"),
        "When button messages do arrive they own click counting, so no click is counted twice");
    catalog.manager.Publish(Oinkster::KeyboardEvent{'A', true, false, true});
    frame(8, 0.27, {'A'}, true);
    Require(!module->DisplayText().contains("[A]"), "Keys held while the ClickGUI is open are not shown");
}

void CoordinatesMatchTheGameReadout() {
    const auto feet = M::FeetFromEye({0.5, 51.62, 0.5});
    RequireNear(feet.y, 50.0, 1e-9, "The camera sits at eye height above the feet");
    Require(M::FormatCoordinate(0.5, true, 1) == "0" && M::FormatCoordinate(50.0, true, 1) == "50"
            && M::FormatCoordinate(-2.98, true, 1) == "-3"
            && M::FormatCoordinate(M::FeetFromEye({0.0, static_cast<double>(51.62F), 0.0}).y, true, 0) == "50",
        "Block coordinates floor to the containing block like the game's own readout");
    Require(M::FormatCoordinate(6.304, false, 2) == "6.30" && M::FormatCoordinate(-0.01, false, 1) == "0.0",
        "Exact coordinates honor precision without a negative zero");

    Catalog catalog;
    catalog.manager.SetCapability(Oinkster::Capability::LocalPlayerPosition, {Oinkster::CapabilityState::Available, {}});
    Require(catalog.manager.SetEnabled("coordinates", true), "Coordinates enable");
    auto* module = catalog.manager.FindHudModule("coordinates");
    Oinkster::PlayerTransform transform{};
    transform.capturedAt = At(0.5);
    transform.position = M::FeetFromEye({0.5, 51.62, 0.5});
    catalog.manager.Publish(transform);
    catalog.manager.Publish(Frame(1, 0.5, 0.016F));
    Require(module->DisplayText().contains("XYZ  0  50  0"), "Spawn at (0.5, 51.62, 0.5) eye reads 0 50 0 like the game");
    catalog.manager.SetModuleSetting("coordinates.mode", "exact");
    Require(module->DisplayText().contains("XYZ  0.5  50.0  0.5"), "Exact mode shows the feet position");
}

void InputLatencyIsInputToNextFrame() {
    M::InputLatencyMeter meter(4);
    meter.OnFrame(At(0.0));
    Require(!meter.LastMilliseconds(), "A frame without input produces no sample");
    meter.OnInput(At(1.000));
    meter.OnInput(At(1.004));
    meter.OnFrame(At(1.012));
    RequireNear(*meter.LastMilliseconds(), 12.0, 1e-6, "Latency runs from the earliest pending input to the next frame");
    meter.OnFrame(At(1.020));
    Require(meter.SampleCount() == 1, "Each input is measured once");
    meter.OnInput(At(2.0));
    meter.OnFrame(At(4.0));
    Require(meter.SampleCount() == 1, "Stalls over one second are discarded, not averaged in");
    for (int i = 0; i < 10; ++i) { meter.OnInput(At(5.0 + i)); meter.OnFrame(At(5.010 + i)); }
    Require(meter.SampleCount() == 4, "Samples are bounded");
    RequireNear(*meter.AverageMilliseconds(), 10.0, 1e-6, "The average covers retained samples");
}

// ------------------------------------------------------------------ status

void StopwatchTransitions() {
    M::Stopwatch stopwatch;
    Require(stopwatch.Current() == M::Stopwatch::State::Reset && stopwatch.Elapsed(At(5)).count() == 0, "Starts reset");
    stopwatch.Pause(At(1));
    Require(stopwatch.Current() == M::Stopwatch::State::Reset, "Pause before start is a no-op");
    stopwatch.Start(At(10));
    Require(stopwatch.Elapsed(At(12.5)).count() == 2500, "Running accumulates");
    stopwatch.Start(At(11));
    Require(stopwatch.Elapsed(At(12.5)).count() == 2500, "Start while running is a no-op");
    stopwatch.Pause(At(13));
    Require(stopwatch.Elapsed(At(100)).count() == 3000, "Paused time does not accumulate");
    stopwatch.Resume(At(20));
    stopwatch.Toggle(At(21));
    Require(stopwatch.Current() == M::Stopwatch::State::Paused && stopwatch.Elapsed(At(50)).count() == 4000, "Resume and toggle-pause");
    stopwatch.Reset();
    Require(stopwatch.Elapsed(At(60)).count() == 0 && stopwatch.Current() == M::Stopwatch::State::Reset, "Reset zeroes");
    Require(M::FormatDuration(std::chrono::milliseconds(3'723'450), true) == "1:02:03.450", "Durations format hours and milliseconds");

    Catalog catalog;
    Require(catalog.manager.SetEnabled("stopwatch", true), "Stopwatch enables");
    Require(catalog.manager.InvokeAction("stopwatch", "start", At(0)), "Start action");
    catalog.manager.Publish(Frame(1, 65, 0.016F));
    Require(catalog.manager.FindHudModule("stopwatch")->DisplayText() == "Stopwatch  1:05", "Elapsed time follows the render clock");
    Require(catalog.manager.FindModule("stopwatch")->Actions().front().id == "pause", "Actions follow state");
    Require(!catalog.manager.InvokeAction("stopwatch", "explode", At(0)), "Unknown actions are refused");
}

void ClockAndSessionStatistics() {
    Require(M::FormatClock(0, 5, 9, false, false) == "12:05 AM" && M::FormatClock(13, 5, 9, false, true) == "1:05:09 PM",
        "12-hour clock with optional seconds");
    Require(M::FormatClock(23, 59, 1, true, true) == "23:59:01" && M::FormatClock(7, 0, 0, true, false) == "07:00", "24-hour clock");

    Catalog catalog;
    Require(catalog.manager.SetEnabled("session-statistics", true), "Session statistics enable");
    auto* module = catalog.manager.FindModule("session-statistics");
    (void)catalog.manager.InvokeAction("session-statistics", "reset", At(0));
    catalog.manager.Publish(Oinkster::KeyboardEvent{'W', true, false, false, At(1)});
    catalog.manager.Publish(Oinkster::KeyboardEvent{'W', true, true, false, At(1.1)});
    catalog.manager.Publish(Oinkster::MouseEvent{0, 0, 0, 1, 1, true, false, At(2)});
    catalog.manager.Publish(Oinkster::MouseEvent{0, 0, 0, 2, 2, true, true, At(2)});
    catalog.manager.Publish(Frame(1, 125, 0.016F));
    const auto text = dynamic_cast<Oinkster::IHudModule*>(module)->DisplayText();
    Require(text.contains("Session  2:05") && text.contains("Keys 1  L 1  R 0") && text.contains("Travelled  --"),
        "Counters exclude repeats and UI-captured input; distance waits for telemetry");
    M::SessionCounters counters;
    counters.Reset(At(0));
    counters.AddPosition({At(0), {0, 64, 0}});
    counters.AddPosition({At(1), {3, 64, 4}});
    counters.AddPosition({At(1.05), {300, 64, 4}});
    counters.AddPosition({At(2), {301, 64, 4}});
    RequireNear(counters.distanceBlocks, 6.0, 1e-9, "Distance sums real steps and skips teleports");
}

void TelemetryDirectionAndSpeed() {
    RequireNear(M::NormalizeYaw360(-90.0F), 270.0, 1e-4, "Negative yaw wraps");
    RequireNear(M::NormalizeYaw360(720.0F), 0.0, 1e-4, "Multiples of 360 wrap to zero");
    RequireNear(M::NormalizeYaw180(270.0F), -90.0, 1e-4, "Signed yaw matches Bedrock's range");
    RequireNear(M::NormalizeYaw180(180.0F), 180.0, 1e-4, "180 stays positive");
    Require(M::NormalizeYaw360(std::nanf("")) == 0.0F, "Non-finite yaw is neutralized");
    Require(M::CompassDirection(0, false) == "South" && M::CompassDirection(90, false) == "West"
        && M::CompassDirection(180, false) == "North" && M::CompassDirection(-90, false) == "East", "Cardinal directions follow Bedrock");
    Require(M::CompassDirection(45, true) == "SW" && M::CompassDirection(135, true) == "NW" && M::CompassDirection(225, true) == "NE"
        && M::CompassDirection(315, true) == "SE" && M::CompassDirection(359, true) == "S", "Intercardinal directions");
    Require(M::FacingAxis(0) == "+Z" && M::FacingAxis(90) == "-X" && M::FacingAxis(180) == "-Z" && M::FacingAxis(270) == "+X", "Facing axes");

    M::SpeedEstimator speed;
    using Result = M::SpeedEstimator::Result;
    Require(speed.Add({At(0.0), {0, 64, 0}}) == Result::Started, "First sample starts the estimator");
    for (int i = 1; i <= 10; ++i) Require(speed.Add({At(0.1 * i), {0.5 * i, 64 + 0.1 * i, 0}}) == Result::Accepted, "Steady movement is accepted");
    RequireNear(*speed.Speed(true), 5.0, 1e-6, "Horizontal speed is path length over time");
    Require(*speed.Speed(false) > *speed.Speed(true), "3D speed includes vertical movement");
    Require(speed.Add({At(1.05), {500, 64, 0}}) == Result::ResetTeleport && !speed.Speed(true), "Teleports reset instead of spiking");
    Require(speed.Add({At(3.0), {501, 64, 0}}) == Result::ResetGap, "Long gaps reset");
    Require(speed.Add({At(3.1), {502, 64, 0}, std::nullopt, std::nullopt, 1}) == Result::ResetDimension, "Dimension changes reset");
    Require(speed.Add({At(3.2), {std::nan(""), 64, 0}}) == Result::Rejected, "Implausible positions are rejected");
    Require(speed.Add({At(3.1), {502, 64, 0}, std::nullopt, std::nullopt, 1}) == Result::Rejected, "Out-of-order samples are rejected");

    M::PingTracker ping;
    ping.Update({At(0), 48U});
    Require(ping.Current(At(1), 5s) == 48U && !ping.Current(At(10), 5s), "Ping goes stale");
    ping.Update({At(11), std::nullopt});
    Require(!ping.Current(At(11), 5s), "A sample without a value is not data");

    constexpr std::uint16_t Empty = M::RakNetEmptyPing;
    Require(M::AveragePing(std::array<std::uint16_t, 5>{40, 50, Empty, 60, Empty}) == 50U, "Average skips empty RakNet ping slots");
    Require(!M::AveragePing(std::array<std::uint16_t, 5>{Empty, Empty, Empty, Empty, Empty}), "An unfilled ring is no ping");
    using Kind = Oinkster::SessionKind;
    Require(M::ClassifyRemoteIpv4({127, 0, 0, 1}) == Kind::Singleplayer, "Loopback is the local world");
    Require(M::ClassifyRemoteIpv4({192, 168, 1, 20}) == Kind::Lan && M::ClassifyRemoteIpv4({10, 1, 2, 3}) == Kind::Lan
        && M::ClassifyRemoteIpv4({172, 20, 0, 1}) == Kind::Lan, "Private IPv4 ranges are LAN");
    Require(M::ClassifyRemoteIpv4({172, 32, 0, 1}) == Kind::Server && M::ClassifyRemoteIpv4({8, 8, 8, 8}) == Kind::Server,
        "Public IPv4 is a server");
    Require(M::ClassifyRemoteIpv6({0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}) == Kind::Singleplayer, "IPv6 loopback");
    Require(M::ClassifyRemoteIpv6({0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF, 192, 168, 0, 9}) == Kind::Lan, "IPv4-mapped IPv6");
    Require(M::ClassifyRemoteIpv6({0xFD, 0x12, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}) == Kind::Lan
        && M::ClassifyRemoteIpv6({0xFE, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}) == Kind::Lan, "ULA and link-local IPv6");
    Require(M::ClassifyRemoteIpv6({0x26, 0x06, 0x47, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}) == Kind::Server, "Global IPv6 is a server");
    Require(M::CleanWorldName("  \xC2\xA7" "6The \xC2\xA7lHive\xC2\xA7r ") == L"The Hive", "World names drop formatting codes and padding");
    Require(M::CleanWorldName(std::string(200, 'a')).size() == M::MaxWorldNameLength, "World names are bounded");

    Catalog catalog;
    catalog.manager.SetCapability(Capability::NetworkMetrics, {CapabilityState::Available, {}});
    Require(catalog.manager.SetEnabled("ping", true), "Ping enables once its adapter validates");
    catalog.manager.Publish(Oinkster::NetworkMetrics{At(0), 47U});
    catalog.manager.Publish(Frame(1, 1, 0.016F));
    Require(catalog.manager.FindHudModule("ping")->DisplayText() == "Ping  47 ms", "Ping formats validated metrics");
    catalog.manager.Publish(Frame(2, 30, 0.016F));
    Require(catalog.manager.FindHudModule("ping")->DisplayText() == "Ping  --", "Stale ping shows as unavailable");
    catalog.manager.SetCapability(Capability::LocalPlayerPosition, {CapabilityState::Available, {}});
    Require(catalog.manager.SetEnabled("coordinates", true), "Coordinates enable");
    catalog.manager.SetModuleSetting("coordinates.mode", "exact");
    catalog.manager.SetModuleSetting("coordinates.precision", "2");
    catalog.manager.Publish(Oinkster::PlayerTransform{At(30), {12.5, 64, -31.255}});
    catalog.manager.Publish(Frame(3, 30.1, 0.016F));
    Require(catalog.manager.FindHudModule("coordinates")->DisplayText() == "XYZ  12.50  64.00  -31.25" ||
            catalog.manager.FindHudModule("coordinates")->DisplayText() == "XYZ  12.50  64.00  -31.26", "Coordinates honor precision");
}

void EquipmentInventoryAndEffects() {
    Oinkster::ItemStack sword{"minecraft:diamond_sword", L"Diamond Sword", 1, 156, 1561};
    Require(M::DurabilityPercent(sword) == 10, "Durability percent rounds");
    Require(!M::DurabilityPercent({"minecraft:stick", L"Stick", 1}), "Items without durability report none");
    Require(!M::DurabilityPercent({"x", L"", 1, 5, 0}) && !M::DurabilityPercent({"x", L"", 1, 9, 5}), "Implausible durability is rejected");

    Oinkster::InventorySnapshot inventory{};
    inventory.slots.resize(80);
    inventory.slots[0] = Oinkster::ItemStack{"minecraft:cobblestone", L"Cobblestone", 64};
    inventory.slots[12] = Oinkster::ItemStack{"minecraft:cobblestone", L"Cobblestone", 20};
    inventory.slots[70] = Oinkster::ItemStack{"minecraft:cobblestone", L"Cobblestone", 64};
    inventory.slots[3] = Oinkster::ItemStack{"minecraft:splash_potion", L"Splash", 2};
    inventory.slots[4] = Oinkster::ItemStack{"minecraft:potion", L"Potion", 1};
    inventory.slots[5] = Oinkster::ItemStack{"minecraft:lingering_potion", L"Lingering", 1};
    Require(M::CountItem(inventory, "minecraft:cobblestone") == 84, "Counting ignores slots past the bound");
    Require(M::SelectedItem(inventory) && M::SelectedItem(inventory)->count == 64, "The selected hotbar slot is read");
    inventory.selectedHotbarSlot = 9;
    Require(!M::SelectedItem(inventory), "A selected slot outside the hotbar is rejected");
    const auto potions = M::CountPotions(inventory);
    Require(potions.drinkable == 1 && potions.splash == 2 && potions.lingering == 1 && potions.Total() == 4, "Potions are counted by kind");

    Oinkster::StatusEffect speed{"minecraft:speed", L"Speed", 1, std::chrono::milliseconds(84'000)};
    Require(M::FormatStatusEffect(speed, true) == "Speed II  1:24", "Effects show name, Roman level and remaining time");
    Require(M::FormatStatusEffect({"minecraft:night_vision", L"", 0, std::nullopt}, true) == "Night vision  **:**", "Infinite effects and id fallback");
    Require(M::EffectLevel(0) == "I" && M::EffectLevel(9) == "X" && M::EffectLevel(254) == "255", "Levels beyond X fall back to numbers");

    Catalog catalog;
    catalog.manager.SetCapability(Capability::Equipment, {CapabilityState::Available, {}});
    catalog.manager.SetCapability(Capability::Effects, {CapabilityState::Available, {}});
    Require(catalog.manager.SetEnabled("armor-status", true) && catalog.manager.SetEnabled("potion-effects", true), "Modules enable");
    Oinkster::EquipmentSnapshot equipment{};
    equipment.armor[0] = Oinkster::ItemStack{"minecraft:iron_helmet", L"Iron Helmet", 1, 150, 165};
    equipment.armor[3] = Oinkster::ItemStack{"minecraft:iron_boots", L"Iron Boots", 1, 20, 195};
    catalog.manager.Publish(equipment);
    const auto armor = catalog.manager.FindHudModule("armor-status")->Content();
    Require(armor.lines.size() == 2 && armor.lines[0].text == "Iron Helmet  91%" && armor.lines[1].tone == Oinkster::HudTone::Warning,
        "Armor status is per slot with low-durability highlighting");
    catalog.manager.Publish(Oinkster::EffectsSnapshot{At(0), {speed, {"minecraft:strength", L"Strength", 0, std::chrono::milliseconds(5000)}}});
    const auto effects = catalog.manager.FindHudModule("potion-effects")->Content();
    Require(effects.lines.size() == 2 && effects.lines[0].text == "Strength  0:05" && effects.lines[0].tone == Oinkster::HudTone::Warning,
        "Effects sort by remaining time and warn when expiring");
}

void EnttRegistryLookupMirrorsTheGame() {
    namespace E = Oinkster::EnttRegistry;
    namespace L = Oinkster::EnttRegistry::Layout;
    Require(E::TypeHash("ActorRotationComponent") == 0x75DF36B7U, "Bare clang type names hash like the 1.26.5203 immediates");
    Require(E::TypeHash("MobEffectsComponent") == 0xE6A1B550U, "MobEffectsComponent hash");

    struct Rotation { float pitch; float yaw; float previousPitch; float previousYaw; };
    constexpr std::uint32_t rotationHash = E::TypeHash("ActorRotationComponent");
    constexpr std::uint32_t version = 3U << 18U;
    constexpr std::uint32_t entityIndex = 5000;   // sparse page 2, slot 904
    constexpr std::uint32_t payloadIndex = 130;   // payload page 1, slot 2

    std::vector<Rotation> payloadPage(L::PayloadPageEntries);
    payloadPage[payloadIndex % L::PayloadPageEntries] = {12.5F, -45.0F, 0.0F, 0.0F};
    std::array<const void*, 2> payloadPages{nullptr, payloadPage.data()};
    std::vector<std::uint32_t> sparsePage(L::SparsePageEntries, 0xFFFFFFFFU);
    sparsePage[entityIndex % L::SparsePageEntries] = version | payloadIndex;
    std::array<const void*, 3> sparsePages{nullptr, nullptr, sparsePage.data()};

    std::array<std::uint8_t, 0x60> storage{};
    auto put = [](auto& buffer, std::size_t offset, auto value) { std::memcpy(buffer.data() + offset, &value, sizeof(value)); };
    put(storage, L::StorageSparseBegin, sparsePages.data());
    put(storage, L::StorageSparseEnd, sparsePages.data() + sparsePages.size());
    put(storage, L::StoragePayloadBegin, payloadPages.data());
    put(storage, L::StoragePayloadEnd, payloadPages.data() + payloadPages.size());

    // Two pools chained in one bucket: the walk must follow node.next.
    std::array<std::uint8_t, 2 * L::PoolNodeSize> nodes{};
    put(nodes, L::PoolNodeNext, static_cast<std::size_t>(-1));
    put(nodes, L::PoolNodeKey, rotationHash);
    put(nodes, L::PoolNodeStorage, static_cast<const void*>(storage.data()));
    put(nodes, L::PoolNodeSize + L::PoolNodeNext, std::size_t{0});
    put(nodes, L::PoolNodeSize + L::PoolNodeKey, E::TypeHash("StateVectorComponent"));
    std::array<std::size_t, 4> buckets;
    buckets.fill(static_cast<std::size_t>(-1));
    buckets[rotationHash & 3U] = 1;

    std::array<std::uint8_t, 0x80> registry{};
    put(registry, L::RegistryPoolBucketsBegin, buckets.data());
    put(registry, L::RegistryPoolBucketsEnd, buckets.data() + buckets.size());
    put(registry, L::RegistryPoolNodesBegin, nodes.data());
    put(registry, L::RegistryPoolNodesEnd, nodes.data() + nodes.size());

    std::array<std::uint8_t, 0x20> actor{};
    put(actor, L::ActorRegistry, static_cast<const void*>(registry.data()));
    put(actor, L::ActorEntity, version | entityIndex);
    E::EntityRef entity;
    Require(E::ReadActorEntity(actor.data(), entity), "Actor EntityContext reads");
    const auto* found = static_cast<const Rotation*>(E::FindComponent(entity, rotationHash, sizeof(Rotation)));
    Require(found == &payloadPage[payloadIndex % L::PayloadPageEntries], "Lookup lands on the payload slot");
    Require(found->yaw == -45.0F, "Component bytes are the payload's");

    std::array<std::uint32_t, 3> packed{version | entityIndex, 0xFFFFFFFFU, (1U << 18U) | 7U};
    put(storage, L::StoragePackedBegin, packed.data());
    put(storage, L::StoragePackedEnd, packed.data() + packed.size());
    std::vector<std::uint32_t> pool;
    Require(E::ReadPoolEntities(registry.data(), rotationHash, 16, pool) && pool.size() == 2
        && pool[0] == (version | entityIndex) && pool[1] == ((1U << 18U) | 7U), "Pool entities skip tombstones");
    Require(E::ReadPoolEntities(registry.data(), rotationHash, 1, pool) && pool.size() == 1, "Pool reads are bounded");
    Require(!E::ReadPoolEntities(registry.data(), E::TypeHash("MobEffectsComponent"), 16, pool) && pool.empty(), "Missing pools read nothing");

    Require(!E::FindComponent({registry.data(), (4U << 18U) | entityIndex}, rotationHash, sizeof(Rotation)), "Stale versions are not members");
    Require(!E::FindComponent({registry.data(), version | 1U}, rotationHash, sizeof(Rotation)), "Entities without a sparse page are not members");
    Require(!E::FindComponent(entity, E::TypeHash("MobEffectsComponent"), 0x18), "Missing pools fail closed");
    buckets[rotationHash & 3U] = 7;
    Require(!E::FindComponent(entity, rotationHash, sizeof(Rotation)), "Out-of-range node indices fail closed");
    std::array<std::size_t, 3> oddBuckets{};
    put(registry, L::RegistryPoolBucketsBegin, oddBuckets.data());
    put(registry, L::RegistryPoolBucketsEnd, oddBuckets.data() + oddBuckets.size());
    Require(!E::FindComponent(entity, rotationHash, sizeof(Rotation)), "Non-power-of-two bucket counts are misreads");
    Require(!E::ReadActorEntity(nullptr, entity), "Null actors fail");
}

void MobEffectRecordsAndRotationPlausibility() {
    const auto speed = M::ToStatusEffect({1, 1680, 1});
    Require(speed && speed->effectId == "minecraft:speed" && speed->amplifier == 1, "Speed II maps by id");
    Require(speed->remaining == std::chrono::milliseconds(84'000), "Ticks are 50 ms");
    const auto infinite = M::ToStatusEffect({16, -1, 0});
    Require(infinite && infinite->effectId == "minecraft:night_vision" && !infinite->remaining, "-1 ticks is infinite");
    Require(M::FormatStatusEffect(*infinite, true) == "Night vision  **:**", "Readable name from the id");
    Require(!M::ToStatusEffect({0, 200, 0}), "Id 0 is the empty slot");
    Require(!M::ToStatusEffect({1, 0, 0}) && !M::ToStatusEffect({1, -5, 0}), "Expired slots are dropped");
    Require(!M::ToStatusEffect({1, 20, -1}) && !M::ToStatusEffect({1, 20, 256}) && !M::ToStatusEffect({65, 20, 0}), "Implausible records are dropped");
    const auto unknown = M::ToStatusEffect({37, 20, 0});
    Require(unknown && unknown->effectId == "minecraft:effect_37", "Unknown ids keep a stable fallback id");

    Require(M::IsPlausibleRotation(-90.0F, 179.9F) && M::IsPlausibleRotation(45.0F, -360.0F), "Valid rotations pass");
    Require(!M::IsPlausibleRotation(91.0F, 0.0F) && !M::IsPlausibleRotation(0.0F, 1e9F)
        && !M::IsPlausibleRotation(std::nanf(""), 0.0F), "Misread rotations fail");
}

// An MSVC std::string as laid out in memory, for building fake game objects.
struct FakeMsvcString {
    char buffer[16]{};
    std::size_t size{};
    std::size_t capacity{15};
    explicit FakeMsvcString(const std::string& text) {
        if (text.size() < 16) {
            std::memcpy(buffer, text.data(), text.size());
        } else {
            const char* heap = text.data();
            std::memcpy(buffer, &heap, sizeof(heap));
            capacity = text.size();
        }
        size = text.size();
    }
};
static_assert(sizeof(FakeMsvcString) == 0x20);

void GameItemsReadStacksAndDamage() {
    namespace G = Oinkster::GameItems;
    namespace L = Oinkster::GameItems::Layout;
    auto put = [](auto& buffer, std::size_t offset, auto value) { std::memcpy(buffer.data() + offset, &value, sizeof(value)); };

    // Item with a heap-allocated raw name and an SSO namespace.
    const std::string longName = "netherite_chestplate";
    std::array<std::uint8_t, 0x140> item{};
    put(item, L::ItemRawName, FakeMsvcString(longName));
    put(item, L::ItemNamespace, FakeMsvcString("minecraft"));
    const void* itemPointer = item.data();
    std::array<const void*, 2> counter{itemPointer, nullptr};

    // CompoundTag { vptr; map head } with a 3-node tree: "Damage" (root),
    // "Count" (left), "RepairCost" (right), so the walk must branch.
    struct Node { std::array<std::uint8_t, 0x70> bytes{}; };
    Node head, damageNode, countNode, repairNode;
    auto key = [&](Node& node, const char* text) { put(node.bytes, L::MapNodeKey, FakeMsvcString(text)); };
    key(damageNode, "Damage");
    key(countNode, "Count");
    key(repairNode, "RepairCost");
    put(head.bytes, L::MapNodeParent, static_cast<const void*>(&damageNode));
    head.bytes[L::MapNodeIsNil] = 1;
    for (Node* leaf : {&countNode, &repairNode}) {
        put(leaf->bytes, L::MapNodeLeft, static_cast<const void*>(&head));
        put(leaf->bytes, L::MapNodeRight, static_cast<const void*>(&head));
    }
    put(damageNode.bytes, L::MapNodeLeft, static_cast<const void*>(&countNode));
    put(damageNode.bytes, L::MapNodeRight, static_cast<const void*>(&repairNode));
    damageNode.bytes[L::MapNodeVariantIndex] = L::VariantIntTag;
    put(damageNode.bytes, L::MapNodeValue + L::IntTagValue, 120);
    repairNode.bytes[L::MapNodeVariantIndex] = L::VariantIntTag;
    put(repairNode.bytes, L::MapNodeValue + L::IntTagValue, 7);
    countNode.bytes[L::MapNodeVariantIndex] = 1;  // ByteTag: not an IntTag
    std::array<std::uint8_t, 0x18> tag{};
    put(tag, L::TagMapHead, static_cast<const void*>(&head));

    int value{};
    Require(G::ReadIntTag(tag.data(), "Damage", value) && value == 120, "Root key");
    Require(G::ReadIntTag(tag.data(), "RepairCost", value) && value == 7, "Right branch");
    Require(!G::ReadIntTag(tag.data(), "Count", value), "Non-IntTag variants are refused");
    Require(!G::ReadIntTag(tag.data(), "Missing", value), "Missing keys reach the nil head");

    std::vector<std::uint8_t> stacks(3 * L::ItemStackSize);
    std::span<std::uint8_t> armored(stacks.data() + L::ItemStackSize, L::ItemStackSize);
    put(armored, L::StackItem, static_cast<const void*>(counter.data()));
    put(armored, L::StackUserData, static_cast<const void*>(tag.data()));
    armored[L::StackCount] = 1;

    Oinkster::Modules::ItemRecord record;
    Require(G::ReadItemRecord(stacks.data(), record) && record.count == 0, "Null item pointers are empty slots");
    Require(G::ReadItemRecord(armored.data(), record), "Stack reads");
    Require(record.name == longName && record.itemNamespace == "minecraft" && record.count == 1 && record.damage == 120, "Fields");
    const auto stack = Oinkster::Modules::ToItemStack(record);
    Require(stack && stack->itemId == "minecraft:netherite_chestplate" && stack->durabilityMax == 592
        && stack->durabilityRemaining == 472, "Durability = vanilla max - Damage");

    std::array<std::uint8_t, 0x1B8> container{};
    put(container, L::SimpleContainerItems, reinterpret_cast<std::uintptr_t>(stacks.data()));
    put(container, L::SimpleContainerItems + 8, reinterpret_cast<std::uintptr_t>(stacks.data() + stacks.size()));
    std::uintptr_t begin{};
    std::size_t count{};
    Require(G::ReadStackVector(container.data(), L::SimpleContainerItems, 64, begin, count) && count == 3, "Vector bounds");
    Require(!G::ReadStackVector(container.data(), L::SimpleContainerItems, 2, begin, count), "Oversized vectors are misreads");
    put(container, L::SimpleContainerItems + 8, reinterpret_cast<std::uintptr_t>(stacks.data() + 5));
    Require(!G::ReadStackVector(container.data(), L::SimpleContainerItems, 64, begin, count), "Unaligned vectors are misreads");
}

void ItemRecordsAndVanillaDurability() {
    namespace S = Oinkster::Modules;
    Require(S::VanillaMaxDurability("minecraft:diamond_sword") == 1561 && S::VanillaMaxDurability("minecraft:wooden_hoe") == 59
        && S::VanillaMaxDurability("minecraft:iron_boots") == 195 && S::VanillaMaxDurability("minecraft:chainmail_helmet") == 165
        && S::VanillaMaxDurability("minecraft:golden_leggings") == 105 && S::VanillaMaxDurability("minecraft:elytra") == 432
        && S::VanillaMaxDurability("minecraft:copper_boots") == 143 && S::VanillaMaxDurability("minecraft:copper_axe") == 190,
        "Vanilla durability table");
    Require(!S::VanillaMaxDurability("minecraft:stick") && !S::VanillaMaxDurability("minecraft:stone_helmet")
        && !S::VanillaMaxDurability("custom:diamond_sword"), "Non-durable and custom items");
    Require(S::IsPlausibleIdentifier("diamond_sword") && !S::IsPlausibleIdentifier("Diamond Sword")
        && !S::IsPlausibleIdentifier("") && !S::IsPlausibleIdentifier("\x01garbage"), "Identifier check");

    const auto stone = S::ToItemStack({"", "cobblestone", 64, std::nullopt});
    Require(stone && stone->itemId == "minecraft:cobblestone" && stone->count == 64 && !stone->durabilityMax, "Default namespace");
    const auto fresh = S::ToItemStack({"minecraft", "bow", 1, std::nullopt});
    Require(fresh && fresh->durabilityRemaining == 384, "Untouched items are at full durability");
    const auto overDamaged = S::ToItemStack({"minecraft", "bow", 1, 999});
    Require(overDamaged && !overDamaged->durabilityMax, "Implausible damage drops durability, not the item");
    Require(!S::ToItemStack({"minecraft", "bow", 0, std::nullopt}) && !S::ToItemStack({"minecraft", "Bow!", 1, std::nullopt}),
        "Empty and garbage stacks");
}

void ServerNamesNeverExposeAddresses() {
    Require(M::LooksLikeAddress(L"192.168.1.20") && M::LooksLikeAddress(L"play.example.net") && M::LooksLikeAddress(L"example:19132")
        && M::LooksLikeAddress(L"fe80::1"), "Addresses and hostnames are detected");
    Require(!M::LooksLikeAddress(L"My Survival World") && !M::LooksLikeAddress(L"Hypixel") && !M::LooksLikeAddress(L"v1.2 world"),
        "Human-chosen names are shown");
    Catalog catalog;
    catalog.manager.SetCapability(Capability::ServerState, {CapabilityState::Available, {}});
    Require(catalog.manager.SetEnabled("server-display", true), "Server display enables");
    catalog.manager.Publish(Oinkster::ServerState{At(0), Oinkster::SessionKind::Server, L"10.0.0.5:19132"});
    Require(catalog.manager.FindHudModule("server-display")->DisplayText() == "Server  hidden address", "Address-like names are masked by default");
    catalog.manager.Publish(Oinkster::ServerState{At(1), Oinkster::SessionKind::Realm, L"Friends Realm"});
    Require(catalog.manager.FindHudModule("server-display")->DisplayText() == "Realm  Friends Realm", "Names show with their session type");
}

// ------------------------------------------------------------------ pvp

void ComboResetRules() {
    M::ComboTracker combo(2s);
    const auto hit = [](double seconds, std::uint64_t target) {
        return Oinkster::CombatEvent{Oinkster::CombatEventKind::LocalHitConfirmed, At(seconds), target};
    };
    combo.OnEvent(hit(0.0, 7));
    combo.OnEvent(hit(0.5, 7));
    combo.OnEvent(hit(1.0, 7));
    Require(combo.Current(At(1.1)) == 3, "Confirmed hits on one target build a combo");
    Require(combo.Current(At(3.5)) == 0, "The combo times out");
    combo.OnEvent(hit(4.0, 7));
    Require(combo.Current(At(4.0)) == 1, "A hit after the timeout starts over");
    combo.OnEvent(hit(4.5, 9));
    Require(combo.Current(At(4.5)) == 1, "Changing target starts a new combo");
    combo.OnEvent(hit(4.8, 9));
    combo.OnEvent({Oinkster::CombatEventKind::LocalDamaged, At(5.0)});
    Require(combo.Current(At(5.0)) == 0, "Taking damage resets the combo");
    combo.OnEvent(hit(5.1, 9));
    combo.OnEvent({Oinkster::CombatEventKind::LocalMissed, At(5.2)});
    Require(combo.Current(At(5.2)) == 0 && combo.Best() == 3, "A reported miss resets; best is kept");
}

void HitDistanceUsesNearestBoundingBoxPoint() {
    const Oinkster::Aabb box{{2.0, 0.0, -0.3}, {2.6, 1.8, 0.3}};
    RequireNear(M::DistanceToAabb({0.0, 1.62, 0.0}, box), 2.0, 1e-9, "Distance runs to the nearest face");
    RequireNear(M::DistanceToAabb({0.0, 3.8, 0.0}, box), std::sqrt(8.0), 1e-9, "Distance runs to the nearest edge");
    RequireNear(M::DistanceToAabb({2.3, 1.0, 0.0}, box), 0.0, 1e-9, "Inside the box is zero");
    RequireNear(M::DistanceToAabb({0.0, 1.0, 0.0}, {{2.6, 1.8, 0.3}, {2.0, 0.0, -0.3}}), 2.0, 1e-9, "Inverted bounds are tolerated");

    const auto south = M::LookDirection(0.0F, 0.0F);
    RequireNear(south.z, 1.0, 1e-9, "Yaw 0 looks south (+Z)");
    const auto west = M::LookDirection(90.0F, 0.0F);
    RequireNear(west.x, -1.0, 1e-9, "Yaw 90 looks west (-X)");
    RequireNear(M::LookDirection(0.0F, 90.0F).y, -1.0, 1e-9, "Positive pitch looks down");
    const Oinkster::Vec3 eye{0.0, 1.0, 0.0};
    const Oinkster::Vec3 east{1.0, 0.0, 0.0};
    RequireNear(*M::RayEntryDistance(eye, east, box), 2.0, 1e-9, "The ray enters the near face");
    Require(!M::RayEntryDistance(eye, {-1.0, 0.0, 0.0}, box), "A ray pointing away misses");
    Require(!M::RayEntryDistance({0.0, 3.0, 0.0}, east, box), "A ray passing above misses");
    RequireNear(*M::RayEntryDistance({2.3, 1.0, 0.0}, east, box), 0.0, 1e-9, "Inside the box is distance zero");

    M::AttackConfirmer confirmer;
    Require(!confirmer.Observe(At(0), 10), "Nothing is confirmed without an armed attack");
    confirmer.Arm({At(1.0), 42, eye, box, 0});
    Require(!confirmer.Observe(At(1.05), 0), "No hurt rise, no hit");
    const auto hit = confirmer.Observe(At(1.2), 10);
    Require(hit && hit->kind == Oinkster::CombatEventKind::LocalHitConfirmed && hit->targetId == 42, "A hurt rise confirms the armed target");
    Require(!confirmer.Observe(At(1.3), 9), "One attack confirms at most once");
    confirmer.Arm({At(2.0), 42, eye, box, 8});
    Require(!confirmer.Observe(At(2.1), 7), "A timer still counting down is not a new hit");
    Require(confirmer.Observe(At(2.2), 10).has_value(), "A rise after counting down is a new hit");
    confirmer.Arm({At(3.0), 7, eye, box, 0});
    Require(!confirmer.Observe(At(4.0), 10) && !confirmer.PendingTarget(At(4.0)), "Armed attacks expire unconfirmed");

    M::HurtRiseWatcher watcher;
    Require(!watcher.Observe(0) && !watcher.Observe(0), "No rise while unhurt");
    Require(watcher.Observe(10) && !watcher.Observe(9) && !watcher.Observe(0), "Only the rise counts");
    Require(!M::IsPlausibleHurtTicks(-1) && !M::IsPlausibleHurtTicks(500) && M::IsPlausibleHurtTicks(10), "Hurt ticks are bounded");
}

void EquipmentWarningsAreNotSpammy() {
    M::EquipmentWarningTracker tracker;
    Oinkster::EquipmentSnapshot snapshot{};
    snapshot.armor[0] = Oinkster::ItemStack{"minecraft:iron_helmet", L"", 1, 30, 165};
    Require(tracker.Evaluate(snapshot, 15, true).empty(), "Healthy equipment does not warn");
    snapshot.armor[0]->durabilityRemaining = 20;
    auto warnings = tracker.Evaluate(snapshot, 15, true);
    Require(warnings.size() == 1 && warnings[0].isNew && warnings[0].slot == "Helmet", "Crossing the threshold warns once");
    warnings = tracker.Evaluate(snapshot, 15, true);
    Require(warnings.size() == 1 && !warnings[0].isNew, "The warning stays without re-alerting");
    snapshot.armor[0]->durabilityRemaining = 30;  // 18%: inside the hysteresis band
    (void)tracker.Evaluate(snapshot, 15, true);
    snapshot.armor[0]->durabilityRemaining = 20;
    Require(!tracker.Evaluate(snapshot, 15, true)[0].isNew, "Hovering at the threshold does not re-alert");
    snapshot.armor[0]->durabilityRemaining = 100;
    (void)tracker.Evaluate(snapshot, 15, true);
    snapshot.armor[0]->durabilityRemaining = 20;
    Require(tracker.Evaluate(snapshot, 15, true)[0].isNew, "Recovery above threshold + hysteresis re-arms");
    snapshot.armor[0] = Oinkster::ItemStack{"minecraft:diamond_helmet", L"", 1, 40, 363};
    Require(tracker.Evaluate(snapshot, 15, true)[0].isNew, "A different item re-arms");
    snapshot.mainHand = Oinkster::ItemStack{"minecraft:bow", L"", 1, 3, 384};
    Require(tracker.Evaluate(snapshot, 15, false).size() == 1, "Held items can be excluded");
}

// ------------------------------------------------------------------ visual

void CrosshairGeometryAndPresentation() {
    M::CrosshairStyle style;
    style.length = 6;
    style.gap = 3;
    style.thickness = 2;
    style.opacity = 0.5F;
    style.outline = true;
    style.centerDot = true;
    const auto primitives = M::BuildCrosshair(style, 1920, 1080);
    Require(primitives.rects.size() == 8 && primitives.circles.size() == 2, "Four arms, four outlines and an outlined dot");
    const auto& left = primitives.rects[4];
    RequireNear(left.x1, 960 - 9, 1e-6, "Arms start gap + length from center");
    RequireNear(left.x2, 960 - 3, 1e-6, "Arms end at the gap");
    Require((left.argb >> 24U) == 128, "Opacity scales alpha");
    Require(primitives.circles[1].x == 960 && primitives.circles[1].y == 540, "The dot is centered");
    style.outline = false;
    style.centerDot = false;
    style.length = 0;
    Require(M::BuildCrosshair(style, 1920, 1080).Empty(), "A zero-length crosshair without a dot draws nothing");
    Require(M::BuildCrosshair(style, 0, 0).Empty(), "A zero-size display draws nothing");

    Catalog catalog;
    Require(catalog.manager.SetEnabled("custom-crosshair", true), "Crosshair enables");
    catalog.manager.SetModuleSetting("custom-crosshair.color", "#FF00FF00");
    const auto modules = catalog.manager.EnabledModulesOf<Oinkster::IOverlayModule>();
    Require(modules.size() == 1 && (modules[0]->Primitives(100, 100).rects.back().argb & 0x00FFFFFFU) == 0x0000FF00U,
        "Crosshair settings reach its geometry");

    Require(!M::CollectPresentationProfile(catalog.manager).fov, "Unavailable presentation modules contribute nothing");
    catalog.manager.SetCapability(Capability::CameraPresentation, {CapabilityState::Available, {}});
    Require(catalog.manager.SetEnabled("fov-profiles", true), "FOV profiles enable once a camera adapter validates");
    catalog.manager.SetModuleSetting("fov-profiles.profile-2", "500");
    (void)catalog.manager.InvokeAction("fov-profiles", "cycle", At(0));
    const auto profile = M::CollectPresentationProfile(catalog.manager);
    Require(profile.fov && profile.fov->profile == 1 && profile.fov->fieldOfView == M::MaximumFov,
        "FOV presets cycle and clamp to the vanilla slider range");
    Require(M::NextFovProfile(2, 3) == 0 && M::NextFovProfile(0, 0) == 0, "Cycling wraps");
}

// ------------------------------------------------------------------ accessibility

void SubtitlesExpireDeduplicateAndOrient() {
    Require(M::CaptionForSound("mob.zombie.say") == "Zombie say", "Mob sounds drop the category");
    Require(M::CaptionForSound("random.explode") == "Explode" && M::CaptionForSound("step.grass") == "Step grass", "Readable captions");
    Require(M::CaptionForSound("music.game").empty() && M::CaptionForSound("random.click").empty() && M::CaptionForSound("ui.cartography_table.take_result").empty(),
        "Music and UI sounds are not captioned");
    Require(M::CaptionForSound("mob.wither.spawn_sound") == "Wither spawn sound" && M::CaptionForSound("").empty(), "Underscores become spaces");
    const Oinkster::Vec3 listener{0, 64, 0};
    Require(M::DirectionTo(listener, 0, {0, 64, 5}) == M::RelativeDirection::Ahead, "Yaw 0 faces +Z");
    Require(M::DirectionTo(listener, 0, {0, 64, -5}) == M::RelativeDirection::Behind, "Behind");
    Require(M::DirectionTo(listener, 0, {-5, 64, 0}) == M::RelativeDirection::Right, "Facing south, west (-X) is to the right");
    Require(M::DirectionTo(listener, 0, {5, 64, 0}) == M::RelativeDirection::Left, "Facing south, east (+X) is to the left");
    Require(M::DirectionTo(listener, 90, {-5, 64, 0}) == M::RelativeDirection::Ahead, "Yaw 90 faces -X");
    Require(M::DirectionTo(listener, 0, listener) == M::RelativeDirection::Unknown, "A sound at the listener has no direction");

    M::SubtitleQueue queue({std::chrono::seconds(3), 3, 16.0});
    const auto sound = [](const char* id, const wchar_t* caption, double seconds, Oinkster::Vec3 at) {
        return Oinkster::SoundEvent{At(seconds), id, caption, at, 1.0F};
    };
    Require(queue.Add(sound("mob.zombie.say", L"Zombie groans", 0, {3, 64, 0}), listener), "Captions are added");
    Require(queue.Add(sound("mob.zombie.say", L"Zombie groans", 1, {4, 64, 0}), listener), "Duplicates refresh");
    Require(queue.Active().size() == 1 && queue.Active()[0].repeats == 2 && queue.Active()[0].position->x == 4, "Duplicates merge");
    Require(!queue.Add(sound("random.explode", L"Explosion", 1, {100, 64, 0}), listener), "Distant sounds are ignored");
    Require(!queue.Add(sound("", L"", 1, {1, 64, 0}), listener), "Sounds without a caption are ignored");
    (void)queue.Add(sound("a", L"Door", 1.5, {1, 64, 0}), listener);
    (void)queue.Add(sound("b", L"Chest", 1.6, {1, 64, 0}), listener);
    (void)queue.Add(sound("c", L"Footsteps", 1.7, {1, 64, 0}), listener);
    Require(queue.Active().size() == 3, "The queue is bounded");
    // Zombie (refreshed at 1 s) expires at 4.0 s - earliest of the four.
    Require(std::ranges::none_of(queue.Active(), [](const M::Caption& c) { return c.text == "Zombie groans"; }),
        "The caption nearest expiry is replaced");
    queue.Expire(At(4.55));
    Require(queue.Active().size() == 2, "Captions expire after their lifetime");
    queue.Expire(At(10));
    Require(queue.Active().empty(), "Every caption eventually expires");
}

void ChatPresentationTransforms() {
    const auto names = M::ParseNameList(L" Steve , Steve_2,,Alex ");
    Require(names.size() == 3 && names.front() == L"Steve_2", "Names parse, trim, and sort longest first");
    Require(M::HideNames(L"steve and Steve_2 met SteveX and Alex.", names, L"Player") == L"Player and Player met SteveX and Player.",
        "Whole-word, case-insensitive replacement without touching longer names");
    Require(M::StripFormattingCodes(L"\u00A7cRed\u00A7r text\u00A7") == L"Red text", "Formatting codes are removed");
    M::ChatPresentation options;
    options.hideNicknames = true;
    options.hiddenNames = names;
    const Oinkster::ChatEvent event{L"hi \u00A7aAlex", L"Steve", {}};
    Require(M::PresentChatLine(event, options, 9, 5) == L"[09:05] <Player> hi Player", "Timestamps, stripping and hiding combine");
    options.timestamps = false;
    options.hideNicknames = false;
    options.stripFormatting = false;
    Require(M::PresentChatLine(event, options, 9, 5) == L"<Steve> hi \u00A7aAlex", "Every transform can be turned off");
}

void SprintAndFramePacingStayWithinPlayerControls() {
    M::SprintController sprint;
    const Oinkster::SprintInputs moving{true, false, true, false, true};
    Require(sprint.WantsSprint(moving), "Always mode sprints while the player holds forward");
    Require(!sprint.WantsSprint({false, false, true, false, true}), "Never without forward held");
    Require(!sprint.WantsSprint({true, true, true, false, true}), "Never while sneaking");
    Require(!sprint.WantsSprint({true, false, false, false, true}), "Never while a game screen owns the cursor");
    Require(!sprint.WantsSprint({true, false, true, true, true}), "Never while the ClickGUI is open");
    Require(!sprint.WantsSprint({true, false, true, false, false}), "Never while unfocused");
    sprint.SetMode(M::SprintController::Mode::Toggle);
    Require(!sprint.WantsSprint(moving), "Toggle mode starts off");
    sprint.OnToggleKey(true);
    Require(sprint.WantsSprint(moving), "The toggle key latches sprint");
    sprint.OnToggleKey(true);
    Require(!sprint.WantsSprint(moving), "Pressing again unlatches");

    const M::FramePacingConfig config{true, 15, 5};
    Require(!M::BackgroundFrameInterval(config, true, false), "Focused games are never throttled");
    Require(M::BackgroundFrameInterval(config, false, false) == std::chrono::microseconds(66'666), "Background cap");
    Require(M::BackgroundFrameInterval(config, false, true) == std::chrono::microseconds(200'000), "Minimized cap");
    Require(!M::BackgroundFrameInterval({false, 15, 5}, false, true), "Dynamic FPS can be turned off independently");
    Require(M::PacingDelay(At(0), At(0.010), std::chrono::microseconds(66'666)) == std::chrono::microseconds(56'666), "Waits out the remainder");
    Require(M::PacingDelay(At(0), At(0.1), std::chrono::microseconds(66'666)).count() == 0, "No wait once the interval has passed");
    Require(M::PacingDelay(At(0), At(0), std::chrono::microseconds(10'000'000)) == M::MaxPacingDelay, "Waits are bounded for prompt unload");

    Catalog catalog;
    Require(catalog.manager.SetEnabled("toggle-sprint-dynamic-fps", true), "The combined module enables");
    auto* assist = catalog.manager.EnabledModulesOf<Oinkster::ISprintAssistModule>().front();
    auto* pacing = catalog.manager.EnabledModulesOf<Oinkster::IFramePacingModule>().front();
    catalog.manager.SetModuleSetting("toggle-sprint-dynamic-fps.sprint-enabled", "false");
    Require(!assist->WantsSprintHeld(moving) && pacing->FrameInterval(false, false).has_value(), "Sprint and dynamic FPS are distinct settings");
    catalog.manager.SetModuleSetting("toggle-sprint-dynamic-fps.sprint-enabled", "true");
    catalog.manager.SetModuleSetting("toggle-sprint-dynamic-fps.dynamic-fps-enabled", "false");
    Require(assist->WantsSprintHeld(moving) && !pacing->FrameInterval(false, false), "Each half toggles independently");
    Require(assist->SprintKey() == VK_CONTROL, "The default sprint key matches Bedrock's default binding");
}

// ------------------------------------------------------------------ infrastructure

void JsonParserIsStrictAndBounded() {
    const auto value = Oinkster::Json::Parse(R"( {"a":[1,-2.5e3,true,null,"\u00e9\ud83d\ude00\n"],"b":{}} )");
    Require(value && value->Find("a")->AsArray()->size() == 5, "Valid documents parse");
    Require(*(*value->Find("a")->AsArray())[4].AsString() == "\xC3\xA9\xF0\x9F\x98\x80\n", "Escapes and surrogate pairs decode to UTF-8");
    for (const char* bad : {"", "{", "[1,]", "{\"a\":1,}", "01", "1.", "\"\\x\"", "\"\x01\"", "[1] 2", "\"\\ud800\"", "nul"}) {
        Require(!Oinkster::Json::Parse(bad), "Malformed JSON is rejected");
    }
    Require(!Oinkster::Json::Parse(std::string(100, '[') + std::string(100, ']')), "Excessive nesting is rejected");
    Require(Oinkster::Json::Escape("a\"b\\c\n\x01") == "a\\\"b\\\\c\\n\\u0001", "Escaping produces valid JSON strings");
    Require(Oinkster::Json::Parse("\xEF\xBB\xBF{}").has_value(), "A UTF-8 BOM is tolerated");
}

void SignatureSelectionFailsClosed() {
    using Oinkster::SignatureScan::Outcome;
    const std::vector<std::vector<std::uintptr_t>> unique{{}, {0x1000}};
    const auto selected = Oinkster::SignatureScan::SelectUnique(unique);
    Require(selected.outcome == Outcome::Unique && selected.address == 0x1000 && selected.patternIndex == 1, "Exactly one match is accepted");
    Require(Oinkster::SignatureScan::SelectUnique(std::vector<std::vector<std::uintptr_t>>{{}, {}}).outcome == Outcome::Missing, "No match fails");
    Require(Oinkster::SignatureScan::SelectUnique(std::vector<std::vector<std::uintptr_t>>{{0x1, 0x2}}).outcome == Outcome::Ambiguous,
        "Two matches from one pattern are ambiguous");
    Require(Oinkster::SignatureScan::SelectUnique(std::vector<std::vector<std::uintptr_t>>{{0x1}, {0x1}}).outcome == Outcome::Ambiguous,
        "Overlapping patterns are ambiguous even at the same address");
    const std::array<std::string_view, 1> invalid{"zz ?"};
    Require(Oinkster::SignatureScan::FindUniqueInText(invalid).outcome == Outcome::InvalidPattern, "Invalid patterns fail closed");
}

void WorldTelemetryRollsBackOutsideMinecraft() {
    Oinkster::ModuleManager manager;
    for (auto& module : Oinkster::CreateModuleCatalog()) manager.Register(std::move(module));
    manager.RestoreEnabled("coordinates", true);
    std::wstring detail;
    Require(!Oinkster::WorldTelemetry::Install(manager, detail), "The telemetry signature must not match a non-Minecraft process");
    const auto status = manager.CapabilityStatusOf(Capability::LocalPlayerPosition);
    Require(status.state == CapabilityState::Unavailable && !status.reason.empty() && status.reason == detail,
        "A failed attach reports the capability unavailable with its reason");
    Require(!manager.IsEnabled("coordinates") && manager.IsRequested("coordinates"), "Nothing activates on a failed attach");
    Oinkster::WorldTelemetry::Remove();
    Oinkster::WorldTelemetry::Remove();
}

void ConcurrentPublishSettingsAndRendering() {
    Catalog catalog;
    auto& manager = catalog.manager;
    for (const auto& entry : Oinkster::CatalogEntries()) (void)manager.SetEnabled(entry.id, true);
    std::atomic_bool stop{};
    std::atomic_int failures{};
    std::thread render([&] {
        for (std::uint64_t frame = 1; frame <= 3000; ++frame) {
            manager.Publish(Frame(frame, static_cast<double>(frame) / 240.0, 1.0F / 240.0F));
            for (auto* hud : manager.EnabledHudModules()) { try { (void)hud->Content(); } catch (...) { ++failures; } }
        }
        stop = true;
    });
    std::thread input([&] {
        for (int i = 0; !stop && i < 200000; ++i) {
            manager.Publish(Oinkster::MouseEvent{0, 0, 0, 1, 1, (i & 1) == 0, false, At(static_cast<double>(i) / 1000.0)});
            manager.Publish(Oinkster::KeyboardEvent{'W', (i & 1) == 0, false, false, At(static_cast<double>(i) / 1000.0)});
        }
    });
    std::thread settings([&] {
        for (int i = 0; !stop && i < 200000; ++i) {
            manager.SetModuleSetting("fps.smoothing-ms", std::to_string(250 + i % 1000));
            manager.SetModuleSetting("cps.show-right", (i & 1) ? "true" : "false");
            (void)manager.SetEnabled("keystrokes", (i & 1) == 0);
            if (i % 100 == 0) manager.ReplaceModuleSettings(manager.ModuleSettings());
        }
    });
    render.join();
    input.join();
    settings.join();
    Require(failures == 0, "Concurrent publish, settings and rendering must not fault");
    Require(catalog.services.frames->Size() == 3000, "Every frame was recorded exactly once");
}
void PreservedSkinAndInputTests() {
        std::vector<std::uint8_t> skin(64 * 64 * 4, 255);
        Require(Oinkster::Dx12Overlay::UpdatePlayerSkin(64, 64, skin, 1), "Valid 64x64 skin must be accepted");
        Require(Oinkster::Dx12Overlay::UpdatePlayerSkin(64, 64, skin, 1),
            "An unchanged skin revision must be accepted without another upload");
        Require(!Oinkster::Dx12Overlay::UpdatePlayerSkin(32, 32, skin, 2), "Unexpected skin dimensions must be rejected");
        auto paint = [](std::vector<std::uint8_t>& atlas, std::uint32_t width,
                         std::uint32_t x, std::uint32_t y, std::array<std::uint8_t, 4> color) {
            const auto offset = (static_cast<std::size_t>(y) * width + x) * 4;
            std::copy(color.begin(), color.end(), atlas.begin() + static_cast<std::ptrdiff_t>(offset));
        };
        std::vector<std::uint8_t> marked64(64 * 64 * 4, 0);
        constexpr std::array<std::array<std::uint32_t, 2>, 6> baseOrigins{{
            {{0, 8}}, {{8, 8}}, {{16, 8}}, {{24, 8}}, {{8, 0}}, {{16, 0}}
        }};
        constexpr std::array<std::array<std::uint32_t, 2>, 6> overlayOrigins{{
            {{32, 8}}, {{40, 8}}, {{48, 8}}, {{56, 8}}, {{40, 0}}, {{48, 0}}
        }};
        for (std::size_t face = 0; face < baseOrigins.size(); ++face) {
            paint(marked64, 64, baseOrigins[face][0], baseOrigins[face][1],
                {static_cast<std::uint8_t>(11 + face), 22, 33, 255});
            paint(marked64, 64, overlayOrigins[face][0], overlayOrigins[face][1],
                {static_cast<std::uint8_t>(77 + face), 88, 99, 128});
        }
        Oinkster::SkinHeadTexture extracted64{};
        Require(Oinkster::ExtractSkinHead(64, 64, marked64, extracted64), "64x64 skin atlas must extract");
        Require(extracted64.faceEdge == 8, "A classic atlas must retain an 8x8 face");
        for (std::size_t face = 0; face < baseOrigins.size(); ++face) {
            Require(extracted64.base[face][0] == (0xFF211600U | static_cast<std::uint32_t>(11 + face)),
                "Every base-face UV must use the classic atlas orientation");
            Require(extracted64.overlay[face][0] == (0x80635800U | static_cast<std::uint32_t>(77 + face)),
                "Every hat-face UV and alpha value must be preserved");
        }
        std::vector<std::uint8_t> marked128(128 * 128 * 4, 0);
        paint(marked128, 128, 16, 16, {9, 8, 7, 255});
        paint(marked128, 128, 17, 16, {30, 20, 10, 255});
        Oinkster::SkinHeadTexture extracted128{};
        Require(Oinkster::ExtractSkinHead(128, 128, marked128, extracted128), "128x128 skin atlas must extract");
        Require(extracted128.faceEdge == 16, "An HD atlas must retain its full 16x16 face");
        Require(extracted128.base[static_cast<std::size_t>(Oinkster::SkinFace::Front)][0] == 0xFF070809U,
            "128x128 UVs must scale from the classic atlas layout");
        Require(extracted128.base[static_cast<std::size_t>(Oinkster::SkinFace::Front)][1] == 0xFF0A141EU,
            "Adjacent HD face pixels must not be discarded by 2x downsampling");
        {
            Oinkster::SkinHeadTexture::Face gradient{};
            gradient[0] = 0xFF000000U;
            gradient[1] = 0xFFFFFFFFU;
            gradient[2] = 0xFF000000U;
            gradient[3] = 0xFFFFFFFFU;
            const auto midpoint = Oinkster::SampleSkinFaceBilinear(gradient, 2, 0.5F, 0.5F);
            const auto midpointRed = midpoint & 0xFFU;
            Require(midpointRed >= 126 && midpointRed <= 128,
                "Bilinear head sampling must smooth transitions instead of drawing pixel blocks");
        }
        Require(!Oinkster::ExtractSkinHead(32, 32, marked64, extracted64), "Invalid skin dimensions must fail closed");
        Require(Oinkster::ParseGuiScale("0.65") == 0.65F, "Configured GUI scale must parse");
        Require(Oinkster::ParseGuiScale("0.1") == 0.50F, "GUI scale must clamp to its minimum");
        Require(Oinkster::ParseGuiScale("broken") == 0.65F, "Invalid GUI scale must use the compact default");
        float velocity60{}, velocity144{};
        float angle60{}, angle144{};
        for (int frame = 0; frame < 60; ++frame) angle60 = Oinkster::StepCriticallyDamped(angle60, velocity60, 24.0F, 1.0F / 60.0F);
        for (int frame = 0; frame < 144; ++frame) angle144 = Oinkster::StepCriticallyDamped(angle144, velocity144, 24.0F, 1.0F / 144.0F);
        Require(std::abs(angle60 - angle144) < 0.01F && angle60 <= 24.001F,
            "Head easing must be frame-rate independent and non-overshooting");
        float spikeVelocity{}, cappedVelocity{};
        const float spike = Oinkster::StepCriticallyDamped(0.0F, spikeVelocity, 24.0F, 0.2F);
        const float capped = Oinkster::StepCriticallyDamped(0.0F, cappedVelocity, 24.0F, 1.0F / 30.0F);
        Require(std::abs(spike - capped) < 0.0001F, "Head easing must cap frame-time spikes");
        Require(!Oinkster::Dx12Overlay::ConsumeEjectRequested(), "Eject must be idle by default");

        Oinkster::KeyStateTracker keyTracker;
        keyTracker.Observe('W', true);
        keyTracker.Observe('A', true);
        keyTracker.Observe('K', true);
        const auto released = keyTracker.BeginIsolation();
        Require(released.size() == 2 && released[0] == 'A' && released[1] == 'W',
            "Opening isolation must release tracked gameplay keys but not the menu key");
        Require(keyTracker.BeginIsolation().empty(), "Opening releases must be emitted exactly once");
        keyTracker.Observe('D', true);
        std::array<bool, 256> physical{};
        physical['W'] = true;
        physical['D'] = true;
        const auto restored = keyTracker.EndIsolation(physical);
        Require(restored.size() == 2 && restored[0] == 'D' && restored[1] == 'W',
            "Closing isolation must restore gameplay keys pressed before or during the menu when still held");
        keyTracker.Reset();

        // The menu/unload keys are configurable, so the tracker must exclude
        // the CONFIGURED keys rather than a hardcoded 'K'. Before this, after
        // rebinding the menu key to 'J' the client released 'J' into the game
        // on open and pushed it back down on close, while 'K' stayed excluded
        // even though it was now an ordinary gameplay key.
        Oinkster::KeyStateTracker reboundTracker;
        reboundTracker.ConfigureControlKeys('J', 'P');
        reboundTracker.Observe('J', true);
        reboundTracker.Observe('P', true);
        reboundTracker.Observe('K', true);
        const auto reboundReleased = reboundTracker.BeginIsolation();
        Require(reboundReleased.size() == 1 && reboundReleased[0] == 'K',
            "Isolation must release the rebound-away key and hold back the configured control keys");

        // --------------------------------------------------------------
        // Live equipped-skin acquisition: the layout-independent halves.
        // --------------------------------------------------------------
        {
            namespace Layout = Oinkster::PlayerSkinLayout;
            using Layout::ImageVerdict;

            // Blob::mBlob is a unique_ptr with a *non-empty* deleter, so MSVC's
            // compressed pair puts the function pointer in slot 0 and the owned
            // pixels in slot 1. That is what places the pixels at Image+0x20
            // and the byte count at Image+0x28 - not +0x18/+0x20. Pinned here
            // because "the Blob is at +0x18, so the pixels are too" is the
            // exact wrong turn that silently reads a deleter as a pixel buffer.
            Require(Layout::ImagePixelsOffset == 0x20,
                "mce::Image pixels must be read from Image+0x20 (Blob+0x18, unique_ptr slot 1)");
            Require(Layout::ImageByteCountOffset == 0x28,
                "mce::Image byte count must be read from Image+0x28");

            // MSVC's release unordered_map keeps _Max_bucket_size at +0x00; the
            // list sentinel is at +0x08. Reading { sentinel, size } from the map
            // base yields { 1.0f, <heap pointer> } and makes every poll bail.
            Require(Layout::MapListHeadOffset == 0x08 && Layout::MapListSizeOffset == 0x10,
                "The release unordered_map list sentinel is at +0x08 and its size at +0x10");
            Require(Layout::NodeEntryOffset == 0x20,
                "PlayerListEntry sits one 16-byte mcUUID key past the list node value");

            Require(Layout::ClassifySkinImage(3, 64, 64, 64ULL * 64 * 4, true) == ImageVerdict::Ok,
                "A 64x64 RGBA atlas must be accepted");
            Require(Layout::ClassifySkinImage(3, 128, 128, 128ULL * 128 * 4, true) == ImageVerdict::Ok,
                "A 128x128 HD RGBA atlas must be accepted");
            Require(Layout::ClassifySkinImage(4, 64, 64, 64ULL * 64 * 4, true) == ImageVerdict::Ok,
                "The newer four-channel skin format value must be accepted");
            Require(Layout::ClassifySkinImage(2, 64, 64, 64ULL * 64 * 4, true) == ImageVerdict::NotRgba,
                "A non-RGBA8 image format must be rejected");
            Require(Layout::ClassifySkinImage(3, 32, 32, 32ULL * 32 * 4, true) == ImageVerdict::UnsupportedExtent,
                "A 32x32 image is not a skin atlas");
            Require(Layout::ClassifySkinImage(3, 64, 128, 64ULL * 128 * 4, true) == ImageVerdict::UnsupportedExtent,
                "A non-square image is not a skin atlas");
            Require(Layout::ClassifySkinImage(3, 64, 64, 1024, true) == ImageVerdict::ByteCountMismatch,
                "A blob whose size disagrees with its dimensions must be rejected");
            Require(Layout::ClassifySkinImage(3, 64, 64, 64ULL * 64 * 4, false) == ImageVerdict::NoPixels,
                "An atlas with no pixel buffer must be rejected");
            Require(Layout::ThreadOwnedSkinAddress(0) == 0,
                "A null SerializedSkinRef owner must stay null");
            Require(Layout::ThreadOwnedSkinAddress(0x12340000) == 0x12340000 + Layout::ThreadOwnerObjectOffset,
                "SerializedSkinRef must unwrap to ThreadOwner::mObject");

            {
                using Variant = Layout::UpdateSkinSignatureVariant;
                const std::array<std::uintptr_t, 1> current{{0x1000}};
                const std::array<std::uintptr_t, 1> legacy{{0x2000}};
                const std::array<std::uintptr_t, 2> duplicate{{0x1000, 0x3000}};
                const std::span<const std::uintptr_t> none{};
                auto selected = Layout::SelectUpdateSkinSignature(current, none);
                Require(selected.variant == Variant::Current && selected.address == 0x1000,
                    "The unique current-build updateSkin signature must be selected");
                selected = Layout::SelectUpdateSkinSignature(none, legacy);
                Require(selected.variant == Variant::Legacy && selected.address == 0x2000,
                    "The unique legacy updateSkin signature must remain supported");
                Require(Layout::SelectUpdateSkinSignature(none, none).variant == Variant::Missing,
                    "No updateSkin matches must report a compatibility miss");
                Require(Layout::SelectUpdateSkinSignature(duplicate, none).variant == Variant::Ambiguous,
                    "Duplicate matches for one signature must fail closed");
                Require(Layout::SelectUpdateSkinSignature(current, legacy).variant == Variant::Ambiguous,
                    "Matches from two signature variants must fail closed");
                Require(Layout::SelectUpdateSkinSignature(current, current).variant == Variant::Ambiguous,
                    "Overlapping signature variants must fail closed even at the same address");
            }

            Require(Layout::IsLocalPlayerTypeName(".?AVLocalPlayer@@"),
                "MSVC LocalPlayer RTTI must identify the authoritative player object");
            Require(Layout::IsLocalPlayerTypeName(".?AVClientLocalPlayer@@"),
                "A current-build LocalPlayer-derived RTTI name must be accepted");
            Require(!Layout::IsLocalPlayerTypeName(".?AVClientPlayer@@"),
                "A generic client player is not sufficient proof of local ownership");
            Require(!Layout::IsLocalPlayerTypeName(".?AVRemotePlayer@@"),
                "Remote-player RTTI must never be accepted as local ownership");
            Require(!Layout::IsLocalPlayerTypeName(".?AVServerPlayer@@"),
                "Server-player RTTI must never be accepted as local ownership");
            {
                using Status = Layout::RetainedOffsetStatus;
                using Kind = Layout::RetainedPointerKind;
                std::array<std::uintptr_t, 6> a{{1, 0xA100, 3, 0, 5, 6}};
                std::array<std::uintptr_t, 6> b{{1, 0xB100, 3, 0, 5, 6}};
                std::array<std::uintptr_t, 6> aAgain{{1, 0xA200, 3, 0, 5, 6}};
                const Layout::RetainedPointerIdentity aId{0xA100, 0xA110};
                const Layout::RetainedPointerIdentity bId{0xB100, 0xB110};
                const Layout::RetainedPointerIdentity aAgainId{0xA200, 0xA210};
                auto selected = Layout::SelectRetainedSkinOffset(a, b, aAgain, aId, bId, aAgainId);
                Require(selected.status == Status::Unique && selected.byteOffset == sizeof(std::uintptr_t)
                        && selected.kind == Kind::ThreadOwner,
                    "A retained ThreadOwner slot must follow A->B->A even when returning to A reallocates it");

                a[1] = aId.skin;
                b[1] = bId.skin;
                aAgain[1] = aAgainId.skin;
                selected = Layout::SelectRetainedSkinOffset(a, b, aAgain, aId, bId, aAgainId);
                Require(selected.status == Status::Unique && selected.kind == Kind::PlayerSkin,
                    "A direct retained PlayerSkin slot must be selectable");

                a[4] = aId.skin;
                b[4] = bId.skin;
                aAgain[4] = aAgainId.skin;
                Require(Layout::SelectRetainedSkinOffset(a, b, aAgain, aId, bId, aAgainId).status
                        == Status::Ambiguous,
                    "Two pointer slots that both follow the sequence must fail closed as ambiguous");

                a.fill(0);
                b.fill(0);
                aAgain.fill(0);
                Require(Layout::SelectRetainedSkinOffset(a, b, aAgain, aId, bId, aAgainId).status
                        == Status::Missing,
                    "A disappearing or absent retained reference must report no selection");
                Require(Layout::SelectRetainedSkinOffset(a, b, aAgain, {}, bId, aAgainId).status
                        == Status::Missing,
                    "A null/malformed identity must fail closed");
            }

            Require(Layout::PlausibleStringFields(5, Layout::StringSsoCapacity),
                "A short std::string keeps exactly the SSO capacity");
            Require(Layout::PlausibleStringFields(40, 47), "A heap std::string may exceed the SSO capacity");
            Require(!Layout::PlausibleStringFields(40, 12), "capacity < size is never a real std::string");
            Require(!Layout::PlausibleStringFields(5, 14),
                "A capacity below the SSO capacity is never a real std::string");
            Require(!Layout::PlausibleStringFields(1ULL << 40, 1ULL << 41),
                "A multi-terabyte std::string is a misread offset");

            Require(Layout::PlausibleSkinFlags(2, 1, 0, 0, 1, 0, 1),
                "A trusted, premium, classic, primary-user skin is a valid flag block");
            Require(!Layout::PlausibleSkinFlags(3, 0, 0, 0, 0, 0, 0),
                "TrustedSkinFlag has no enumerator 3");
            Require(!Layout::PlausibleSkinFlags(0, 0, 0, 0, 0, 0, 4),
                "persona::ArmSize::Type has no enumerator 4");
            Require(!Layout::PlausibleSkinFlags(0, 0, 2, 0, 0, 0, 0),
                "A bool holding 2 means this is not a PlayerSkin");

            // Eight buckets of two iterators each: mask == 7, span == 0x80.
            Require(Layout::PlausiblePlayerMapShape(3, 0x2000, 0x1000, 0x1080, 0x1080, 7),
                "A well-formed eight-bucket player map must validate");
            Require(!Layout::PlausiblePlayerMapShape(3, 0, 0x1000, 0x1080, 0x1080, 7),
                "A null list sentinel must be rejected");
            Require(!Layout::PlausiblePlayerMapShape(0, 0x2000, 0x1000, 0x1080, 0x1080, 7),
                "An empty player map carries no skins");
            Require(!Layout::PlausiblePlayerMapShape(Layout::MaxPlayerMapEntries + 1, 0x2000,
                        0x1000, 0x1080, 0x1080, 7),
                "An implausible player count means the offset is wrong");
            Require(!Layout::PlausiblePlayerMapShape(3, 0x2000, 0x1000, 0x1080, 0x1080, 6),
                "_Mask must be one less than the bucket count");
            Require(!Layout::PlausiblePlayerMapShape(3, 0x2000, 0x1000, 0x1060, 0x1060, 5),
                "A bucket count that is not a power of two must be rejected");
            Require(!Layout::PlausiblePlayerMapShape(3, 0x2000, 0x1000, 0x1088, 0x1088, 7),
                "A bucket span that is not a whole number of buckets must be rejected");
            Require(!Layout::PlausiblePlayerMapShape(3, 0x2000, 0x1080, 0x1000, 0x1080, 7),
                "An inverted bucket vector must be rejected");
            Require(!Layout::PlausiblePlayerMapShape(3, 0x2000, 0x1000, 0x1080, 0x1040, 7),
                "A _Mylast past _Myend must be rejected");

            Require(Layout::ChooseSkinCandidate({}) == Layout::NoCandidate,
                "No candidates means no selection");
            {
                // Another player's classic skin first, then our own - the old
                // reader took whatever matched first and rendered a stranger.
                const std::array<Layout::SkinCandidate, 2> candidates{{
                    {0x1000, 0x9000, 64, 64, false, false, true},
                    {0x2000, 0xA000, 64, 64, true, false, true},
                }};
                Require(Layout::ChooseSkinCandidate(candidates) == 1,
                    "mIsPrimaryUser must win over an earlier non-primary entry");
            }
            {
                const std::array<Layout::SkinCandidate, 2> candidates{{
                    {0x1000, 0x9000, 64, 64, true, false, false},
                    {0x2000, 0xA000, 64, 64, true, false, true},
                }};
                Require(Layout::ChooseSkinCandidate(candidates) == 1,
                    "Among primary users, the authoritative player map must win over a heap find");
            }
            {
                const std::array<Layout::SkinCandidate, 2> candidates{{
                    {0x1000, 0x9000, 64, 64, true, true, true},
                    {0x2000, 0xA000, 64, 64, true, false, true},
                }};
                Require(Layout::ChooseSkinCandidate(candidates) == 1,
                    "A classic atlas must win over a persona atlas for the same player");
            }
            {
                // A persona skin still qualifies when it is all there is.
                const std::array<Layout::SkinCandidate, 1> candidates{{
                    {0x1000, 0x9000, 128, 128, true, true, true},
                }};
                Require(Layout::ChooseSkinCandidate(candidates) == 0,
                    "A persona skin with raw pixels must still be usable");
                Require(Layout::IsPrimaryUpdateCandidate(candidates[0]),
                    "A primary persona skin from Player::updateSkin must be accepted");
            }
            {
                const Layout::SkinCandidate remote{0x1000, 0x9000, 64, 64, false, false, false};
                Require(!Layout::IsPrimaryUpdateCandidate(remote),
                    "A remote Player::updateSkin callback must not replace the local head");
            }
            {
                const std::array<Layout::SkinCandidate, 2> candidates{{
                    {0x2000, 0x9000, 64, 64, true, false, true},
                    {0x1000, 0xA000, 64, 64, true, false, true},
                }};
                Require(Layout::ChooseSkinCandidate(candidates) == 1,
                    "Equally ranked candidates must resolve to the lowest address so scans agree");
            }
            {
                const std::array<Layout::SkinCandidate, 1> candidates{{
                    {0x1000, 0x9000, 33, 33, true, false, true},
                }};
                Require(Layout::ChooseSkinCandidate(candidates) == Layout::NoCandidate,
                    "A candidate with an unsupported extent must never be selected");
            }

            const std::array<std::uint8_t, 4> opaque{{1, 2, 3, 255}};
            const std::array<std::uint8_t, 4> other{{1, 2, 4, 255}};
            Require(Layout::HashPixels(opaque) != 0, "A pixel revision must never be zero");
            Require(Layout::HashPixels(opaque) == Layout::HashPixels(opaque),
                "Identical pixels must produce an identical revision");
            Require(Layout::HashPixels(opaque) != Layout::HashPixels(other),
                "Different pixels must produce a different revision");
            {
                std::array<std::uint8_t, 64 * 64 * 4> validSkin{};
                for (std::size_t y = 8; y < 16; ++y) {
                    for (std::size_t x = 8; x < 16; ++x) {
                        validSkin[(y * 64 + x) * 4 + 3] = 255;
                    }
                }
                Require(Layout::HasUsableHeadPixels(validSkin, 64, 64),
                    "An opaque classic base-layer face must be accepted");
                validSkin.fill(0);
                validSkin[(8 * 64 + 8) * 4 + 3] = 255;
                Require(!Layout::HasUsableHeadPixels(validSkin, 64, 64),
                    "A nearly transparent scratch atlas must not be cached as a player skin");
                Require(!Layout::HasUsableHeadPixels(validSkin, 128, 128),
                    "Pixel storage must agree with the atlas dimensions");
            }
        }
}

void PreservedShutdownTests() {
        // --------------------------------------------------------------
        // Status pipe shutdown must be prompt and repeatable.
        //
        // The worker previously sat in a blocking, uncancellable
        // ConnectNamedPipe, and Stop() tried to wake it by connecting a
        // throwaway client before an untimed join(). If that connect landed
        // between the worker closing one pipe instance and creating the next,
        // the wake was lost and Stop() blocked forever. It is now driven by an
        // overlapped accept plus a stop event (Oinkster/PipeWait.hpp), so this
        // completes immediately - and would hang the whole test binary if the
        // regression came back.
        // --------------------------------------------------------------
        {
            using namespace std::chrono;
            for (int iteration = 0; iteration < 5; ++iteration) {
                Oinkster::StatusPipe pipe;
                Require(pipe.Start(L"{\"interfaceVersion\":2,\"state\":\"validating\"}"),
                    "The status pipe must start");
                Require(!pipe.Start(L"{}"), "A second Start on a running pipe must be refused");
                pipe.Update(L"{\"interfaceVersion\":2,\"state\":\"ready\"}");

                const auto startedAt = steady_clock::now();
                pipe.Stop();
                const auto elapsed = duration_cast<milliseconds>(steady_clock::now() - startedAt);
                Require(elapsed < seconds(5),
                    "Stopping the status pipe must not block waiting for a client");

                // Stop() is called from both the runtime's shutdown path and
                // the destructor, so it has to be idempotent.
                pipe.Stop();
            }
        }

        // The unload guard latches on first Block() and keeps that reason.
        Oinkster::UnloadGuard::ResetForTesting();
        Require(!Oinkster::UnloadGuard::Blocked(), "The unload guard must start clear");
        Oinkster::UnloadGuard::Block("first reason");
        Oinkster::UnloadGuard::Block("second reason");
        Require(Oinkster::UnloadGuard::Blocked(), "Block must latch");
        Require(Oinkster::UnloadGuard::Reason() == "first reason",
            "The first recorded reason must win so the original cause is not lost");
        Oinkster::UnloadGuard::ResetForTesting();
        Require(!Oinkster::UnloadGuard::Blocked(), "Reset must clear the guard");
}

}

int main() {
    const std::array<std::pair<const char*, void (*)()>, 43> tests{{
        {"CatalogIdsAreUniqueAndWellFormed", CatalogIdsAreUniqueAndWellFormed},
        {"NativeCatalogMatchesLauncherJson", NativeCatalogMatchesLauncherJson},
        {"AvailabilityFollowsCapabilities", AvailabilityFollowsCapabilities},
        {"ModuleLifecycleIsIsolated", ModuleLifecycleIsIsolated},
        {"ClickGuiViewsAndFavorites", ClickGuiViewsAndFavorites},
        {"SettingParsingAndNormalization", SettingParsingAndNormalization},
        {"ModuleSettingsRoundTripThroughManager", ModuleSettingsRoundTripThroughManager},
        {"SettingsDocumentRoundTripsAndMigrates", SettingsDocumentRoundTripsAndMigrates},
        {"SettingsApplyAndCapturePreserveDeferredModules", SettingsApplyAndCapturePreserveDeferredModules},
        {"KeybindConflictsAndReservedKeys", KeybindConflictsAndReservedKeys},
        {"RollingFpsAndFrameTime", RollingFpsAndFrameTime},
        {"OnePercentLowIsTheSlowestPercentNotTheMinimum", OnePercentLowIsTheSlowestPercentNotTheMinimum},
        {"GraphHistoryIsBounded", GraphHistoryIsBounded},
        {"PerformanceModulesShareOneFrameHistory", PerformanceModulesShareOneFrameHistory},
        {"CpsUsesIndependentRollingWindows", CpsUsesIndependentRollingWindows},
        {"KeystrokesTrackOnlyGameInput", KeystrokesTrackOnlyGameInput},
        {"KeystrokesShowsMouseCps", KeystrokesShowsMouseCps},
        {"KeystrokesFollowPhysicalKeyState", KeystrokesFollowPhysicalKeyState},
        {"CoordinatesMatchTheGameReadout", CoordinatesMatchTheGameReadout},
        {"InputLatencyIsInputToNextFrame", InputLatencyIsInputToNextFrame},
        {"StopwatchTransitions", StopwatchTransitions},
        {"ClockAndSessionStatistics", ClockAndSessionStatistics},
        {"TelemetryDirectionAndSpeed", TelemetryDirectionAndSpeed},
        {"EquipmentInventoryAndEffects", EquipmentInventoryAndEffects},
        {"EnttRegistryLookupMirrorsTheGame", EnttRegistryLookupMirrorsTheGame},
        {"MobEffectRecordsAndRotationPlausibility", MobEffectRecordsAndRotationPlausibility},
        {"GameItemsReadStacksAndDamage", GameItemsReadStacksAndDamage},
        {"ItemRecordsAndVanillaDurability", ItemRecordsAndVanillaDurability},
        {"ServerNamesNeverExposeAddresses", ServerNamesNeverExposeAddresses},
        {"ComboResetRules", ComboResetRules},
        {"HitDistanceUsesNearestBoundingBoxPoint", HitDistanceUsesNearestBoundingBoxPoint},
        {"EquipmentWarningsAreNotSpammy", EquipmentWarningsAreNotSpammy},
        {"CrosshairGeometryAndPresentation", CrosshairGeometryAndPresentation},
        {"SubtitlesExpireDeduplicateAndOrient", SubtitlesExpireDeduplicateAndOrient},
        {"ChatPresentationTransforms", ChatPresentationTransforms},
        {"SprintAndFramePacingStayWithinPlayerControls", SprintAndFramePacingStayWithinPlayerControls},
        {"JsonParserIsStrictAndBounded", JsonParserIsStrictAndBounded},
        {"SignatureSelectionFailsClosed", SignatureSelectionFailsClosed},
        {"WorldTelemetryRollsBackOutsideMinecraft", WorldTelemetryRollsBackOutsideMinecraft},
        {"ConcurrentPublishSettingsAndRendering", ConcurrentPublishSettingsAndRendering},
        {"PreservedSkinAndInputTests", PreservedSkinAndInputTests},
        {"PreservedShutdownTests", PreservedShutdownTests},
        {"ModuleLifecycleIsIsolated (repeat)", ModuleLifecycleIsIsolated},
    }};
    int failed = 0;
    for (const auto& [name, test] : tests) {
        try {
            test();
        } catch (const std::exception& error) {
            ++failed;
            std::cerr << "FAIL " << name << ": " << error.what() << '\n';
        } catch (...) {
            ++failed;
            std::cerr << "FAIL " << name << ": unknown exception\n";
        }
    }
    if (failed != 0) {
        std::cerr << failed << " of " << tests.size() << " native tests failed\n";
        return 1;
    }
    std::cout << "All " << tests.size() << " native tests passed\n";
    return 0;
}
