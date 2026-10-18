#include "Oinkster/ModuleManager.hpp"
#include "Oinkster/ModuleCatalog.hpp"
#include "Oinkster/SettingsStore.hpp"
#include "Oinkster/SignatureScan.hpp"
#include "Oinkster/SkinHead.hpp"
#include "Oinkster/PlayerSkinLayout.hpp"
#include "Oinkster/RakNet.hpp"
#include "Oinkster/ReplayBlockPack.hpp"
#include "Oinkster/ReplayCameraPath.hpp"
#include "Oinkster/ReplayCapture.hpp"
#include "Oinkster/ReplayFile.hpp"
#include "Oinkster/ReplayPlayback.hpp"
#include "Oinkster/ReplayInjector.hpp"
#include "Oinkster/ReplayTimeline.hpp"
#include "Oinkster/ReplayWorld.hpp"
#include "Oinkster/ClickGui.hpp"
#include "Oinkster/CrosshairTexture.hpp"
#include "Oinkster/Dx12Overlay.hpp"
#include "Oinkster/EnttRegistry.hpp"
#include "Oinkster/GameItems.hpp"
#include "Oinkster/InputState.hpp"
#include "Oinkster/Json.hpp"
#include "Oinkster/PostProcess.hpp"
#include "Oinkster/StatusPipe.hpp"
#include "Oinkster/ServerPolicy.hpp"
#include "Oinkster/UnloadGuard.hpp"
#include "Oinkster/VideoRecorder.hpp"
#include "Oinkster/WorldTelemetry.hpp"
#include "Oinkster/Modules/AccessibilityLogic.hpp"
#include "Oinkster/Modules/FrameStatistics.hpp"
#include "Oinkster/Modules/HudFormat.hpp"
#include "Oinkster/Modules/InputLogic.hpp"
#include "Oinkster/Modules/PacketLogic.hpp"
#include "Oinkster/Modules/HiveLogic.hpp"
#include "Oinkster/PacketSender.hpp"
#include "Oinkster/Modules/PvpLogic.hpp"
#include "Oinkster/Modules/CameraMath.hpp"
#include "Oinkster/Modules/StatusLogic.hpp"
#include "Oinkster/Modules/TelemetryLogic.hpp"
#include "Oinkster/Modules/VisualLogic.hpp"
#include <WinSock2.h>
#include <Windows.h>
#include <array>
#include <atomic>
#include <bit>
#include <bitset>
#include <format>
#include <chrono>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
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
    Require(catalog.manager.Size() == 60, "Catalog must contain 60 modules");
    std::unordered_set<std::string> ids;
    std::vector<std::string> defaultEnabled;
    for (const auto& entry : Oinkster::CatalogEntries()) {
        const std::string id(entry.id);
        Require(ids.insert(id).second, "Catalog ids must be unique");
        Require(!id.empty() && std::ranges::all_of(id, [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'; }),
            "Catalog ids must be lowercase kebab-case");
        Require(catalog.manager.FindModule(id) != nullptr, "Every catalog entry must be registered");
        Require((catalog.manager.FindHudModule(id) != nullptr) == entry.hud, "The HUD flag must match the module's interface");
        if (catalog.manager.FindModule(id)->Metadata().defaultEnabled) defaultEnabled.push_back(id);
    }
    Require(defaultEnabled == std::vector<std::string>{"debug-menu"}, "Only the debug menu is enabled by default");
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

void ClickFiltersAreDeterministicAndFailSafe() {
    M::ClickDebouncer debounce;
    debounce.Configure(20ms, 30ms, false);
    Require(debounce.Allow(1, At(1.000)), "The first left click is accepted");
    Require(!debounce.Allow(1, At(1.019)), "A left click inside the debounce interval is discarded");
    Require(debounce.Allow(1, At(1.020)), "A click exactly on the debounce boundary is accepted");
    Require(debounce.Allow(2, At(1.000)), "Buttons have independent debounce histories");
    Require(!debounce.Allow(2, At(1.029)), "The right button uses its own interval");
    Require(debounce.Allow(2, At(0.500)), "A backwards timestamp resets safely instead of locking input");
    debounce.Configure(25ms, 100ms, true);
    debounce.Clear();
    Require(debounce.Allow(2, At(2.000)) && !debounce.Allow(2, At(2.024)) && debounce.Allow(2, At(2.025)),
        "Linked debounce uses the left interval for both buttons");

    M::CpsLimiter limiter;
    limiter.Configure(2, 1);
    Require(limiter.Allow(1, At(3.000)) && limiter.Allow(1, At(3.100)), "Clicks through the left cap are accepted");
    Require(!limiter.Allow(1, At(3.200)), "The first click above the rolling cap is discarded");
    Require(limiter.Allow(1, At(4.000)), "An event exactly one second old leaves the rolling window");
    Require(limiter.Allow(2, At(3.000)) && !limiter.Allow(2, At(3.500)), "Right CPS has an independent limit");
    Require(limiter.Allow(2, At(2.000)), "A backwards timestamp clears the CPS window safely");
}

void CompetitiveServerPolicyFailsClosed() {
    Catalog catalog;
    auto& manager = catalog.manager;
    manager.SetCapability(Capability::InputFiltering, {CapabilityState::Available, {}});

    Oinkster::ServerPolicy::Apply(manager, Oinkster::SessionKind::None, Oinkster::ServerNetwork::Local);
    Require(!manager.Availability("dc-prevent").available && !manager.Availability("cps-cap").available,
        "An unclassified or stale session must fail closed even without a remote address");

    Oinkster::ServerPolicy::Apply(manager, Oinkster::SessionKind::Singleplayer, Oinkster::ServerNetwork::Local);
    Require(manager.Availability("dc-prevent").available && manager.Availability("cps-cap").available,
        "Both click filters are permitted in singleplayer");

    Oinkster::ServerPolicy::Apply(manager, Oinkster::SessionKind::Server, Oinkster::ServerNetwork::Zeqa);
    Require(manager.Availability("dc-prevent").available, "Zeqa explicitly permits its DC Prevent behavior");
    Require(!manager.Availability("cps-cap").available, "Zeqa CPS Cap remains blocked without explicit approval");

    for (const auto network : {Oinkster::ServerNetwork::Hive, Oinkster::ServerNetwork::CubeCraft,
        Oinkster::ServerNetwork::Galaxite, Oinkster::ServerNetwork::Lifeboat,
        Oinkster::ServerNetwork::NetherGames, Oinkster::ServerNetwork::Mineville,
        Oinkster::ServerNetwork::UnknownRemote}) {
        Oinkster::ServerPolicy::Apply(manager, Oinkster::SessionKind::Server, network);
        Require(!manager.Availability("dc-prevent").available && !manager.Availability("cps-cap").available,
            "Unapproved remote networks must block both filters");
    }

    Require(Oinkster::ServerPolicy::ClassifyHostname(L"na.zeqa.net.") == Oinkster::ServerNetwork::Zeqa,
        "Canonical subdomains and a trailing DNS dot classify without exposing the hostname");
    Require(Oinkster::ServerPolicy::ClassifyHostname("geo.hivebedrock.network") == Oinkster::ServerNetwork::Hive,
        "Known featured-server hostnames classify");
    Require(Oinkster::ServerPolicy::ClassifyHostname("notzeqa.net.example") == Oinkster::ServerNetwork::UnknownRemote,
        "Suffix lookalikes fail closed");
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

void SettingsProfilesSaveSwitchAndRemove() {
    const auto directory = std::filesystem::current_path() / "test-artifacts" / "OinksterProfiles";
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
    Oinkster::SettingsProfileStore profiles(directory);
    Require(Oinkster::SettingsProfileStore::IsValidName("Bedwars")
        && Oinkster::SettingsProfileStore::IsValidName("Low end PC"), "Ordinary profile names are accepted");
    Require(!Oinkster::SettingsProfileStore::IsValidName("../escape")
        && !Oinkster::SettingsProfileStore::IsValidName("CON"), "Unsafe profile filenames are rejected");

    Oinkster::ClientSettings settings;
    settings.moduleStates["fps"] = true;
    profiles.Save("Bedwars", settings);
    settings.moduleStates["fps"] = false;
    profiles.Save("Creative", settings);
    profiles.SetActive("Bedwars");

    Require(profiles.List() == std::vector<std::string>({"Bedwars", "Creative"}),
        "Profiles are listed alphabetically");
    Require(profiles.Active() == "Bedwars" && profiles.Load("Bedwars")->moduleStates.at("fps"),
        "The active profile and its complete settings round-trip");
    Require(profiles.Remove("Creative") && !profiles.Load("Creative"), "A saved profile can be removed");
    profiles.SetActive({});
    Require(profiles.Active().empty(), "The active profile can be cleared");
    std::filesystem::remove_all(directory, ignored);
}

void SettingsApplyAndCapturePreserveDeferredModules() {
    Catalog defaults;
    defaults.manager.SetCapability(Capability::LocalPlayerPosition, {CapabilityState::Available, {}});
    Oinkster::ClientSettings noPersistedState;
    Oinkster::ApplySettings(defaults.manager, noPersistedState);
    Require(defaults.manager.IsEnabled("debug-menu"), "A missing persisted state uses the debug menu's enabled default");
    noPersistedState.moduleStates["debug-menu"] = false;
    Oinkster::ApplySettings(defaults.manager, noPersistedState);
    Require(!defaults.manager.IsEnabled("debug-menu"), "An explicit disabled state wins over the catalog default");

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

void CrosshairSourceAndPerspective() {
    using Oinkster::CameraPerspective;
    using M::CrosshairPerspectiveFilter;
    using M::CrosshairSource;
    constexpr std::array perspectives{CameraPerspective::Unknown, CameraPerspective::FirstPerson,
        CameraPerspective::ThirdPersonBack, CameraPerspective::ThirdPersonFront};
    for (const auto perspective : perspectives) {
        for (const auto filter : {CrosshairPerspectiveFilter::FirstPersonOnly, CrosshairPerspectiveFilter::AllPerspectives}) {
            Require(!M::ShouldDrawCustomCrosshair(CrosshairSource::Game, filter, perspective),
                "The game-default source never draws a custom crosshair");
        }
        Require(M::ShouldDrawCustomCrosshair(CrosshairSource::Custom, CrosshairPerspectiveFilter::AllPerspectives, perspective),
            "All perspectives draws in every perspective");
    }
    Require(M::ShouldDrawCustomCrosshair(CrosshairSource::Custom, CrosshairPerspectiveFilter::FirstPersonOnly, CameraPerspective::FirstPerson),
        "First-person-only draws in first person");
    Require(M::ShouldDrawCustomCrosshair(CrosshairSource::Custom, CrosshairPerspectiveFilter::FirstPersonOnly, CameraPerspective::Unknown),
        "First-person-only falls back to drawing while the perspective is unknown");
    Require(!M::ShouldDrawCustomCrosshair(CrosshairSource::Custom, CrosshairPerspectiveFilter::FirstPersonOnly, CameraPerspective::ThirdPersonBack)
        && !M::ShouldDrawCustomCrosshair(CrosshairSource::Custom, CrosshairPerspectiveFilter::FirstPersonOnly, CameraPerspective::ThirdPersonFront),
        "First-person-only suppresses a known third-person perspective");
    Require(M::ParseCrosshairSource("game") == CrosshairSource::Game && M::ParseCrosshairSource("custom") == CrosshairSource::Custom
        && M::ParseCrosshairSource("") == CrosshairSource::Custom && M::ParseCrosshairSource("vanilla") == CrosshairSource::Custom,
        "Crosshair source parses, defaulting to custom");
    Require(M::ParseCrosshairPerspective("all") == CrosshairPerspectiveFilter::AllPerspectives
        && M::ParseCrosshairPerspective("") == CrosshairPerspectiveFilter::FirstPersonOnly,
        "Crosshair perspective parses, defaulting to first person only");

    M::CrosshairConfig config;
    Require(M::BuildCrosshairFrame(config, CameraPerspective::Unknown, 1920, 1080).rects.size() == 8,
        "Default config draws four outlined arms");
    config.source = CrosshairSource::Game;
    Require(M::BuildCrosshairFrame(config, CameraPerspective::FirstPerson, 1920, 1080).Empty(),
        "Game default produces no primitives");

    // The live module: an older settings file has neither new key.
    Catalog catalog;
    auto& manager = catalog.manager;
    Require(manager.SettingsFor("custom-crosshair").Choice("source") == "custom"
        && manager.SettingsFor("custom-crosshair").Choice("perspective") == "first-person",
        "Existing configurations load as a first-person custom crosshair");
    Require(manager.SetEnabled("custom-crosshair", true), "Crosshair enables");
    const auto overlay = [&manager] {
        const auto modules = manager.EnabledModulesOf<Oinkster::IOverlayModule>();
        return modules.empty() ? Oinkster::OverlayPrimitives{} : modules[0]->Primitives(1920, 1080);
    };
    Require(manager.EnabledModulesOf<Oinkster::IOverlayModule>().size() == 1, "Exactly one crosshair overlay is active");
    const auto centered = overlay();
    Require(centered.rects.size() == 8, "Custom draws the configured crosshair");
    const auto& top = centered.rects[6];
    RequireNear((top.x1 + top.x2) * 0.5, 960, 1e-6, "Vertical arms are centered horizontally");
    RequireNear(top.y2, 540 - 3, 1e-6, "The top arm ends at the gap above center");

    manager.SetModuleSetting("custom-crosshair.source", "game");
    Require(overlay().Empty(), "Selecting the game default stops custom primitives");
    manager.SetModuleSetting("custom-crosshair.source", "bogus");
    Require(manager.ModuleSetting("custom-crosshair.source", "") == "custom" && !overlay().Empty(),
        "An unknown source normalizes back to custom");

    manager.Publish(Oinkster::CameraPerspectiveEvent{At(1), CameraPerspective::ThirdPersonBack});
    Require(overlay().Empty(), "First-person-only suppresses the crosshair in third person");
    manager.SetModuleSetting("custom-crosshair.perspective", "all");
    Require(!overlay().Empty(), "All perspectives draws in third person");
    manager.SetModuleSetting("custom-crosshair.perspective", "first-person");
    manager.Publish(Oinkster::CameraPerspectiveEvent{At(2), CameraPerspective::FirstPerson});
    Require(!overlay().Empty(), "Returning to first person draws again");
    manager.Publish(Oinkster::CameraPerspectiveEvent{At(3), CameraPerspective::ThirdPersonFront});

    Require(manager.SetEnabled("custom-crosshair", false) && manager.EnabledModulesOf<Oinkster::IOverlayModule>().empty(),
        "Disabling the module removes it from the overlay path immediately");
    Require(overlay().Empty(), "A disabled crosshair produces no primitives");
    Require(manager.SetEnabled("custom-crosshair", true) && !overlay().Empty(),
        "Re-enabling forgets the stale third-person state");
    manager.SetModuleSetting("custom-crosshair.opacity", "0.5");
    manager.SetModuleSetting("custom-crosshair.center-dot", "true");
    Require((overlay().rects.back().argb >> 24U) == 128 && overlay().circles.size() == 2,
        "Customization still reaches the geometry");
}

void CrosshairDrawnAndGameCopy() {
    using Oinkster::CameraPerspective;
    using M::CrosshairSource;

    // Pixel-grid settings round-trip and normalize.
    const auto plus = M::DefaultCrosshairPixels();
    const auto text = Oinkster::FormatPixelGrid(plus);
    Require(text.size() == 64 && Oinkster::ParsePixelGrid(text) == plus, "A pixel grid round-trips as 64 hex digits");
    Require(text.substr(0, 4) == "0000" && text.substr(7 * 4, 4) == "1FF8" && text.substr(3 * 4, 4) == "0180",
        "The default plus is ten cells long and two thick");
    Require(!Oinkster::ParsePixelGrid("XYZ") && !Oinkster::ParsePixelGrid(std::string(63, '0')), "Malformed grids are refused");
    const auto pixelSetting = M::PixelSetting("pixels", "Draw", plus);
    Require(Oinkster::NormalizeSetting(pixelSetting, "garbage") == text, "A malformed grid falls back to the default");
    Require(Oinkster::NormalizeSetting(pixelSetting, std::string(64, 'f')) == std::string(64, 'F'), "Grids normalize to uppercase");
    Oinkster::PixelGrid left{};
    Oinkster::SetPixel(left, 0, 0, true);
    Require(Oinkster::PixelAt(Oinkster::MirrorPixelsHorizontally(left), 15, 0)
        && Oinkster::PixelAt(Oinkster::MirrorPixelsVertically(left), 0, 15), "Mirroring copies the first half onto the second");

    // Drawn geometry: one rect per run, centred, outlines first.
    M::PixelCrosshairStyle style;
    style.pixels = plus;
    style.scale = 2;
    style.outline = false;
    const auto drawn = M::BuildPixelCrosshair(style, 1920, 1080);
    Require(drawn.rects.size() == 10, "Runs merge: one rect per row of the plus");
    const auto& row7 = drawn.rects[7 - 3];
    RequireNear(row7.x1, 960 - 10, 1e-6, "The grid centre sits on the screen centre");
    RequireNear(row7.x2, 960 + 10, 1e-6, "A ten-cell run is twenty pixels at scale 2");
    style.outline = true;
    const auto outlined = M::BuildPixelCrosshair(style, 1920, 1080);
    Require(outlined.rects.size() == 20 && (outlined.rects.front().argb & 0x00FFFFFFU) == 0, "Outlines precede fills");
    style.pixels = {};
    Require(M::BuildPixelCrosshair(style, 1920, 1080).Empty(), "An empty grid draws nothing");

    // The game texture redrawn: transparent texels skipped, equal runs merged.
    Oinkster::ArgbImage image{4, 2, {0, 0xFFFFFFFFU, 0xFFFFFFFFU, 0, 0x80FF0000U, 0, 0, 0}};
    const auto copy = M::BuildImageCrosshair(image, 2, 1, 100, 100);
    Require(copy.rects.size() == 2, "One rect per opaque run");
    RequireNear(copy.rects[0].x1, 50 - 4 + 2, 1e-6, "The texture is centred");
    Require(copy.rects[1].argb == 0x80FF0000U, "Texel colors are kept");
    Oinkster::ArgbImage large{256, 256, std::vector<std::uint32_t>(256 * 256, 0xFFFFFFFFU)};
    const auto sampled = M::BuildImageCrosshair(large, 1, 1, 1920, 1080);
    Require(sampled.rects.size() == 64, "Large textures are sampled down to 64 per side");
    Require(M::BuildImageCrosshair(Oinkster::ArgbImage{2, 2, {1}}, 1, 1, 100, 100).Empty(), "A malformed image draws nothing");

    // Only a known third-person camera redraws the game's crosshair.
    for (const auto perspective : {CameraPerspective::Unknown, CameraPerspective::FirstPerson}) {
        Require(!M::ShouldDrawGameCrosshairCopy(CrosshairSource::Game, true, perspective),
            "The game's own crosshair is never doubled in first person or when unknown");
    }
    Require(M::ShouldDrawGameCrosshairCopy(CrosshairSource::Game, true, CameraPerspective::ThirdPersonBack)
        && M::ShouldDrawGameCrosshairCopy(CrosshairSource::Game, true, CameraPerspective::ThirdPersonFront),
        "Third person redraws the game's crosshair");
    Require(!M::ShouldDrawGameCrosshairCopy(CrosshairSource::Game, false, CameraPerspective::ThirdPersonBack)
        && !M::ShouldDrawGameCrosshairCopy(CrosshairSource::Drawn, true, CameraPerspective::ThirdPersonBack),
        "Only the enabled game source redraws it");

    // The live module.
    Catalog catalog;
    auto& manager = catalog.manager;
    Require(manager.SetEnabled("custom-crosshair", true), "Crosshair enables");
    const auto overlay = [&manager] {
        const auto modules = manager.EnabledModulesOf<Oinkster::IOverlayModule>();
        return modules.empty() ? Oinkster::OverlayPrimitives{} : modules[0]->Primitives(1920, 1080);
    };
    manager.SetModuleSetting("custom-crosshair.source", "drawn");
    Require(manager.ModuleSetting("custom-crosshair.source", "") == "drawn" && overlay().rects.size() == 20,
        "Drawn shows the default plus with an outline");
    manager.SetModuleSetting("custom-crosshair.pixels", std::string(64, '0'));
    Require(overlay().Empty(), "A cleared drawing draws nothing");

    manager.SetModuleSetting("custom-crosshair.source", "game");
    const auto texture = std::make_shared<const Oinkster::ArgbImage>(image);
    manager.Publish(Oinkster::GameCrosshairEvent{At(1), texture});
    Require(overlay().Empty(), "Game source draws nothing while the perspective is unknown");
    manager.Publish(Oinkster::CameraPerspectiveEvent{At(2), CameraPerspective::ThirdPersonBack});
    Require(overlay().rects.size() == 2, "Third person redraws the pack's crosshair");
    manager.SetModuleSetting("custom-crosshair.third-person", "false");
    Require(overlay().Empty(), "Turning the third-person copy off stops it");
    manager.SetModuleSetting("custom-crosshair.third-person", "true");
    manager.Publish(Oinkster::CameraPerspectiveEvent{At(3), CameraPerspective::FirstPerson});
    Require(overlay().Empty(), "First person leaves the game's crosshair alone");
    manager.Publish(Oinkster::CameraPerspectiveEvent{At(4), CameraPerspective::ThirdPersonFront});
    manager.Publish(Oinkster::GameCrosshairEvent{At(5), nullptr});
    Require(overlay().Empty(), "An unknown texture draws nothing");

    // Settings shown for each source.
    const auto* module = manager.FindModule("custom-crosshair");
    const auto visible = [&](std::string_view key) {
        const auto reader = manager.SettingsFor("custom-crosshair");
        for (const auto& descriptor : module->Settings()) {
            if (descriptor.key == key) return Oinkster::IsSettingVisible(descriptor, reader);
        }
        return false;
    };
    Require(visible("source") && visible("third-person") && !visible("pixels") && !visible("color"),
        "Game shows only its own settings");
    manager.SetModuleSetting("custom-crosshair.source", "drawn");
    Require(visible("pixels") && visible("color") && !visible("gap") && !visible("third-person"), "Drawn shows the grid and colors");
    manager.SetModuleSetting("custom-crosshair.source", "custom");
    Require(visible("gap") && visible("color") && !visible("pixels"), "Custom shows the line settings");
}

void GameCrosshairTextureSource() {
    namespace T = Oinkster::CrosshairTexture;

    // A two-entry archive in Bedrock's .brarchive layout.
    const std::string first = "first.png";
    const std::string second = "cross_hair.png";
    std::vector<std::uint8_t> archive{0x7D, 0x27, 0x25, 0xB1, 0xA0, 0x52, 0x70, 0x26, 2, 0, 0, 0, 1, 0, 0, 0};
    const auto addEntry = [&archive](const std::string& name, std::uint32_t offset, std::uint32_t size) {
        std::vector<std::uint8_t> entry(256, 0);
        entry[0] = static_cast<std::uint8_t>(name.size());
        std::copy(name.begin(), name.end(), entry.begin() + 1);
        std::memcpy(entry.data() + 248, &offset, 4);
        std::memcpy(entry.data() + 252, &size, 4);
        archive.insert(archive.end(), entry.begin(), entry.end());
    };
    addEntry(first, 0, 3);
    addEntry(second, 3, 2);
    archive.insert(archive.end(), {'a', 'b', 'c', 'X', 'Y'});
    const auto found = T::FindBrarchiveEntry(archive, "cross_hair.png");
    Require(found && found->size() == 2 && (*found)[0] == 'X' && (*found)[1] == 'Y', "A named brarchive entry is found");
    Require(!T::FindBrarchiveEntry(archive, "missing.png"), "A missing entry is not found");
    auto truncated = archive;
    truncated.resize(truncated.size() - 1);
    Require(!T::FindBrarchiveEntry(truncated, "cross_hair.png"), "An entry past the end of the archive is refused");
    archive[0] = 0;
    Require(!T::FindBrarchiveEntry(archive, "first.png"), "A wrong magic is refused");

    Require(T::FormatPackUuid(0x0123456789ABCDEFULL, 0xFEDCBA9876543210ULL) == "01234567-89ab-cdef-fedc-ba9876543210",
        "Pack uuids format like the game's own");
    Require(T::ManifestHeaderUuid(R"({ // comment
        "format_version": 2, /* block */ "header": {"name": "Pack // not a comment", "uuid": "AAAAAAAA-0000-1111-2222-333333333333"}})")
            == std::optional<std::string>("aaaaaaaa-0000-1111-2222-333333333333"),
        "Manifest uuids are read past comments and lowercased");
    Require(!T::ManifestHeaderUuid(R"({"header": {"uuid": "short"}})") && !T::ManifestHeaderUuid("not json"),
        "Malformed manifests have no uuid");

    constexpr std::array<std::uint8_t, 72> png{0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x48,
        0x44, 0x52, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01, 0x08, 0x06, 0x00, 0x00, 0x00, 0xF4, 0x22, 0x7F, 0x8A, 0x00, 0x00,
        0x00, 0x0F, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9C, 0x63, 0xF8, 0xCF, 0xC0, 0xF0, 0x1F, 0x08, 0x1B, 0x00, 0x10, 0x79, 0x03, 0x7E,
        0x7D, 0x63, 0xCE, 0xD7, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82};
    const auto decoded = T::DecodePng(png);
    Require(decoded && decoded->width == 2 && decoded->height == 1 && decoded->pixels[0] == 0xFFFF0000U
        && decoded->pixels[1] == 0x8000FF00U, "PNGs decode to ARGB");
    Require(!T::DecodePng(std::span<const std::uint8_t>(png.data(), 20)) && !T::DecodePng(std::span<const std::uint8_t>(archive)),
        "Truncated and non-PNG data does not decode");
}

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

void CameraMathMatchesGameConvention() {
    namespace CM = M::CameraMath;
    const CM::Quat identity{};
    const auto forward = CM::Forward(identity);
    Require(forward.z == -1.0F && forward.x == 0.0F && forward.y == 0.0F, "An identity camera looks down -Z");

    // Yaw 30 degrees left, pitch 20 degrees up, then a 15 degree roll.
    const auto yawPitch = CM::OffsetYawPitch(identity, 30.0F * CM::Pi / 180.0F, 20.0F * CM::Pi / 180.0F);
    RequireNear(CM::Pitch(yawPitch) * 180.0F / CM::Pi, 20.0, 1e-3, "Pitch offset looks up");
    RequireNear(CM::Forward(yawPitch).x, -std::sin(30.0F * CM::Pi / 180.0F) * std::cos(20.0F * CM::Pi / 180.0F), 1e-4,
        "Positive yaw turns left");
    const auto rolled = CM::Multiply(yawPitch, CM::AxisAngle({0.0F, 0.0F, 1.0F}, 15.0F * CM::Pi / 180.0F));
    RequireNear(CM::Right(rolled).y, std::sin(15.0F * CM::Pi / 180.0F) * std::cos(20.0F * CM::Pi / 180.0F), 1e-4, "Roll tilts the right axis");

    const auto level = CM::RollFree(rolled);
    RequireNear(CM::Right(level).y, 0.0, 1e-4, "RollFree levels the right axis");
    RequireNear(CM::Dot(CM::Forward(level), CM::Forward(rolled)), 1.0, 1e-4, "RollFree keeps the view direction");
    RequireNear(std::abs(CM::Dot(CM::ScaleRoll(rolled, 1.0F), rolled)), 1.0, 1e-5, "Full roll strength is vanilla");
    RequireNear(CM::Right(CM::ScaleRoll(rolled, 0.5F)).y, std::sin(7.5F * CM::Pi / 180.0F) * std::cos(20.0F * CM::Pi / 180.0F), 2e-3, "Half strength halves the roll");

    const auto clamped = CM::OffsetYawPitch(identity, 0.0F, 3.0F);
    RequireNear(CM::Pitch(clamped), CM::MaxPitchRadians, 1e-3, "Freelook pitch stops short of straight up");
    RequireNear(std::abs(CM::Dot(CM::Slerp(identity, yawPitch, 0.0F), identity)), 1.0, 1e-6, "Slerp starts at its origin");
    RequireNear(std::abs(CM::Dot(CM::Slerp(identity, yawPitch, 1.0F), yawPitch)), 1.0, 1e-6, "Slerp ends at its target");
    RequireNear(CM::DistanceAlongView({0.0F, 0.0F, 4.0F}, {0.0F, 0.0F, -1.0F}, {0.5F, 0.2F, 0.0F}), 4.0, 1e-6,
        "Distance along the view ignores sideways offset");

    // Looking straight at the south face of block (0, 0, 0) from 2.5 blocks away.
    Oinkster::BlockSelectionEvent selection;
    selection.capturedAt = At(0);
    selection.hasBlock = true;
    selection.face = Oinkster::BlockFace::South;
    selection.cameraPosition = {0.5F, 0.5F, 3.5F};
    selection.verticalFov = 70.0F * CM::Pi / 180.0F;
    const M::BlockOutlineRequest style{0xFFFF0000U, 2.0F, 0.25F};
    const auto outline = M::BuildBlockOutline(style, selection, 1920.0F, 1080.0F);
    Require(outline.lines.size() == 4, "Only the edges of the face toward the camera are drawn");
    Require(outline.quads.size() == 1 && ((outline.quads[0].argb >> 24U) == 63 || (outline.quads[0].argb >> 24U) == 64), "The looked-at face is filled at the fill opacity");
    float centerX = 0.0F;
    for (const float x : outline.quads[0].x) centerX += x / 4.0F;
    RequireNear(centerX, 960.0, 0.5, "A block straight ahead projects to the screen center");
    selection.hasBlock = false;
    Require(M::BuildBlockOutline(style, selection, 1920.0F, 1080.0F).Empty(), "No hit block draws nothing");
}

void VisualPresentationModulesCompose() {
    Catalog catalog;
    for (const auto capability : {Capability::CameraPresentation, Capability::CameraRotation, Capability::CameraMotion,
        Capability::ThirdPersonPresentation, Capability::LightingPresentation, Capability::TimePresentation,
        Capability::WeatherPresentation, Capability::ScreenEffectsPresentation, Capability::HandPresentation}) {
        catalog.manager.SetCapability(capability, {CapabilityState::Available, {}});
    }
    for (const auto* id : {"fov-profiles", "zoom", "freelook", "cinematic-camera", "third-person-distance",
        "fullbright", "time-changer", "weather-suppression", "screen-overlay-suppression", "hurt-camera", "hand-view"}) {
        Require(catalog.manager.SetEnabled(id, true), "Presentation module enables once its capability validates");
    }

    catalog.manager.SetModuleSetting("fov-profiles.profile-1", "90");
    catalog.manager.SetModuleSetting("zoom.fov", "1");
    catalog.manager.SetModuleSetting("third-person-distance.distance", "999");
    catalog.manager.SetModuleSetting("fullbright.multiplier", "999");
    catalog.manager.SetModuleSetting("time-changer.ticks", "99999");
    catalog.manager.SetModuleSetting("weather-suppression.rain", "25");
    catalog.manager.SetModuleSetting("screen-overlay-suppression.portal-opacity", "40");

    auto profile = M::CollectPresentationProfile(catalog.manager);
    RequireNear(M::EffectiveFov(profile, 80), 90, 1e-6, "FOV profile applies while zoom is idle");
    Require(profile.cameraRotation && !profile.cameraRotation->active && profile.cinematicCamera,
        "Freelook state and cinematic smoothing compose independently");
    Require(profile.thirdPersonDistance && profile.thirdPersonDistance->distance == 20.0F,
        "Third-person distance is normalized");
    RequireNear(M::EffectiveThirdPersonDistance(profile, Oinkster::CameraPerspective::ThirdPersonBack, 4.0F), 20.0F,
        1e-6, "Third-person distance applies to an enabled known perspective");
    RequireNear(M::EffectiveThirdPersonDistance(profile, Oinkster::CameraPerspective::FirstPerson, 4.0F), 4.0F,
        1e-6, "Third-person distance never applies in first person");
    RequireNear(M::EffectiveThirdPersonDistance(profile, Oinkster::CameraPerspective::Unknown, 4.0F), 4.0F,
        1e-6, "Third-person distance fails closed while perspective is unknown");
    Require(profile.lighting && profile.lighting->multiplier == 10.0F && profile.time && profile.time->ticks == 23999,
        "Lighting and time settings clamp to their maximums");
    Require(profile.weather && profile.weather->rain == 0.25F && profile.screenEffects && profile.screenEffects->portal == 0.4F,
        "Weather and screen-effect channels stay independent");
    Require(profile.hurtCamera && profile.hurtCamera->strength == 0.0F && profile.handView,
        "Hurt-camera and hand presentation contribute their defaults");

    (void)catalog.manager.PublishInput(Oinkster::KeyboardEvent{'C', true, false});
    (void)catalog.manager.PublishInput(Oinkster::KeyboardEvent{0x12, true, false});
    profile = M::CollectPresentationProfile(catalog.manager);
    Require(profile.zoom && M::EffectiveFov(profile, 80) == M::MinimumZoomFov,
        "Active zoom overrides FOV profiles and supports the requested deep-zoom minimum");
    Require(profile.cameraRotation && profile.cameraRotation->active, "Freelook activation reaches the presentation profile");
    (void)catalog.manager.PublishInput(Oinkster::KeyboardEvent{'C', false, false});
    (void)catalog.manager.PublishInput(Oinkster::KeyboardEvent{0x12, false, false});
    profile = M::CollectPresentationProfile(catalog.manager);
    Require(!profile.zoom && profile.cameraRotation && !profile.cameraRotation->active,
        "Hold-mode camera requests restore their vanilla path on key-up");
    Require(catalog.manager.SetEnabled("fov-profiles", false), "FOV profiles disable cleanly");
    RequireNear(M::EffectiveFov(M::CollectPresentationProfile(catalog.manager), 82.0F), 82.0F, 1e-6,
        "Disabling the final FOV contributor restores vanilla immediately");
    catalog.manager.SetCapability(Capability::ThirdPersonPresentation,
        {CapabilityState::Unavailable, L"test capability loss"});
    Require(!M::CollectPresentationProfile(catalog.manager).thirdPersonDistance,
        "Losing a capability removes its presentation contribution immediately");
}

void ToggleSneakResetsOutsideGameplay() {
    Catalog catalog;
    Require(catalog.manager.SetEnabled("toggle-sneak", true), "Toggle sneak uses already-reviewed local input synthesis");
    auto* assist = catalog.manager.EnabledModulesOf<Oinkster::IKeyHoldAssistModule>().front();
    auto* hud = dynamic_cast<Oinkster::IHudModule*>(assist);
    Require(hud && hud->Content().lines.empty(), "Toggle sneak is a HUD module hidden while released");
    const Oinkster::HeldKeyInputs gameplay{true, false, true};
    Require(assist->DesiredHeldKey(gameplay) == 0, "Toggle sneak starts released");
    (void)catalog.manager.PublishInput(Oinkster::KeyboardEvent{'X', true, false});
    Require(assist->DesiredHeldKey(gameplay) == VK_SHIFT, "Toggle sneak requests the configured game key");
    Require(hud->Content().bare && hud->Content().lines.size() == 1
        && hud->Content().lines.front().text == "[Sneaking (Toggled)]",
        "Toggle sneak shows the Java-style active indicator without a panel");
    Require(assist->DesiredHeldKey({false, true, true}) == 0, "A game screen releases and resets toggle sneak");
    Require(assist->DesiredHeldKey(gameplay) == 0, "Toggle sneak stays reset after gameplay resumes");
    Require(hud->Content().lines.empty(), "The indicator disappears when gameplay resets the latch");
    (void)catalog.manager.PublishInput(Oinkster::KeyboardEvent{'X', true, false});
    Require(assist->DesiredHeldKey(gameplay) == VK_SHIFT, "Toggle sneak can latch again");
    Require(catalog.manager.SetEnabled("toggle-sneak", false), "Toggle sneak disables");
    Require(assist->DesiredHeldKey(gameplay) == 0, "Disabling the module resets its held state");
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

void ReplayFileRoundTripsAndRecovers() {
    namespace Replay = Oinkster::Replay;
    const auto root = std::filesystem::temp_directory_path() / "OinksterReplayTests";
    const auto path = root / "round-trip.oinkreplay";
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);

    Replay::Writer writer;
    std::string error;
    Require(writer.Open(path, 1'700'000'000'123ULL, error), "Replay writer must open");
    Require(writer.AppendMetadata(R"({"minecraftVersion":"1.26.5203.0","protocol":0})", error),
        "Replay metadata must append");
    const std::array<std::uint8_t, 5> packet{{0x09, 0x01, 0x02, 0x03, 0x04}};
    Require(writer.Append(Replay::RecordKind::IncomingPacket, 12'345, packet, error),
        "Incoming packet must append");

    // Before Finalize, the readable prefix is deliberately left beside the
    // destination so a crash loses only the last incomplete record.
    const auto partial = writer.PartialPath();
    const auto recovered = Replay::Read(partial);
    Require(recovered.error.empty(), "A live partial replay prefix must be readable");
    Require(!recovered.cleanEnd && recovered.records.size() == 2,
        "A partial replay must report recovery rather than a clean end");

    if (!writer.Finalize(99'000, error)) {
        throw std::runtime_error("Replay finalization must publish the completed file: " + error);
    }
    Require(std::filesystem::exists(path) && !std::filesystem::exists(partial),
        "Finalization must replace the partial file with the destination");
    const auto replay = Replay::Read(path);
    Require(replay.error.empty() && replay.cleanEnd, "Final replay must have a valid clean end");
    Require(replay.startedUnixMillis == 1'700'000'000'123ULL, "Replay start time must round-trip");
    Require(replay.records.size() == 2 && replay.records[1].timestampMicros == 12'345,
        "Replay records and timestamps must round-trip");
    Require(replay.records[1].payload == std::vector<std::uint8_t>(packet.begin(), packet.end()),
        "Packet bytes must round-trip exactly");

    // A truncated tail is recoverable, while corrupted committed bytes are
    // rejected instead of being fed back into the game.
    const auto truncatedPath = root / "truncated.oinkreplay";
    std::filesystem::copy_file(path, truncatedPath, std::filesystem::copy_options::overwrite_existing);
    std::filesystem::resize_file(truncatedPath, std::filesystem::file_size(truncatedPath) - 8);
    const auto truncated = Replay::Read(truncatedPath);
    Require(truncated.error.empty() && !truncated.cleanEnd && truncated.records.size() == 2,
        "A torn final record must preserve the committed prefix");

    auto bytes = ReadFile(path);
    Require(bytes.size() > 64, "Replay fixture must contain record payload bytes");
    bytes[64] = static_cast<char>(static_cast<unsigned char>(bytes[64]) ^ 0x80U);
    const auto corruptPath = root / "corrupt.oinkreplay";
    {
        std::ofstream output(corruptPath, std::ios::binary | std::ios::trunc);
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
    Require(!Replay::Read(corruptPath).error.empty(), "A corrupt replay record must be rejected");
    std::filesystem::remove_all(root, ignored);
}

void AppendReplayText(Oinkster::Replay::Writer& writer, Oinkster::Replay::RecordKind kind,
    std::uint64_t timestamp, std::string_view text) {
    std::string error;
    const auto bytes = std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
    Require(writer.Append(kind, timestamp, bytes, error), "Replay fixture record must append");
}

void ReplayTimelineBuildsSamplesAndMarks() {
    namespace Replay = Oinkster::Replay;
    const auto root = std::filesystem::temp_directory_path() / "OinksterReplayTimelineTests";
    const auto path = root / "timeline.oinkreplay";
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);

    Replay::Writer writer;
    std::string error;
    Require(writer.Open(path, 1'700'000'000'000ULL, error), "Replay writer must open");
    Require(writer.AppendMetadata(R"({"format":"oinkster-replay"})", error), "Metadata must append");
    AppendReplayText(writer, Replay::RecordKind::Camera, 0,
        R"({"x":1.0,"y":64.0,"z":-2.0,"yaw":90.0,"pitch":0.0,"dimension":0})");
    AppendReplayText(writer, Replay::RecordKind::Marker, 50'000, "mark 1");
    AppendReplayText(writer, Replay::RecordKind::Camera, 100'000,
        R"({"x":11.0,"y":65.0,"z":8.0,"yaw":null,"pitch":null,"dimension":null})");
    AppendReplayText(writer, Replay::RecordKind::Marker, 150'000, "pause");
    Require(writer.Finalize(400'000, error), "Replay must finalize");

    const auto timeline = Replay::BuildTimeline(Replay::Read(path));
    Require(timeline.error.empty() && timeline.cleanEnd, "Timeline must come from a clean replay");
    Require(timeline.startedUnixMillis == 1'700'000'000'000ULL, "Start time must carry over");
    Require(timeline.durationMicros == 150'000, "An unfinished pause removes the remaining wall-clock time");
    Require(timeline.samples.size() == 2, "Both camera records must become samples");
    Require(!timeline.samples[1].yaw && !timeline.samples[1].dimension, "null fields must stay absent");
    Require(timeline.marks.size() == 1 && timeline.marks[0].label == "mark 1",
        "Playback controls stay out of the visible highlight marks");
    RequireNear(timeline.minX, 1.0, 1e-9, "Bounds must cover the path (min x)");
    RequireNear(timeline.maxX, 11.0, 1e-9, "Bounds must cover the path (max x)");
    RequireNear(timeline.minZ, -2.0, 1e-9, "Bounds must cover the path (min z)");
    RequireNear(timeline.maxZ, 8.0, 1e-9, "Bounds must cover the path (max z)");

    const auto summary = Replay::Summarize(path);
    Require(summary.error.empty() && summary.cleanEnd && summary.sampleCount == 2 && summary.markCount == 1,
        "Summary must count samples and highlight marks only");
    Require(summary.thumbnail.size() == 2 && summary.bytes > 0, "Summary must carry a thumbnail path and size");
    std::filesystem::remove_all(root, ignored);
}

void ReplayTimelineCollapsesRecordingPauses() {
    namespace Replay = Oinkster::Replay;
    const auto text = [](Replay::RecordKind kind, std::uint64_t t, std::string_view value) {
        return Replay::Record{kind, t, std::vector<std::uint8_t>(value.begin(), value.end())};
    };
    Replay::ReadResult replay;
    replay.records = {
        text(Replay::RecordKind::Marker, 1'000, "clip-start"),
        text(Replay::RecordKind::Camera, 1'100, R"({"x":1,"y":2,"z":3})"),
        text(Replay::RecordKind::Marker, 1'200, "pause"),
        text(Replay::RecordKind::Camera, 2'000, R"({"x":9,"y":9,"z":9})"),
        text(Replay::RecordKind::Marker, 3'200, "resume"),
        text(Replay::RecordKind::Camera, 3'300, R"({"x":4,"y":5,"z":6})"),
        text(Replay::RecordKind::Marker, 3'400, "mark 1"),
    };
    replay.cleanEnd = true;
    replay.endTimestampMicros = 4'000;

    const auto clock = Replay::BuildPlaybackClock(replay);
    Require(clock.clipStartMicros == 1'000 && clock.endMicros == 2'000,
        "The playback clock removes a completed recording pause");
    Require(clock.Map(2'000) == 1'200 && clock.Map(3'300) == 1'300,
        "Paused state changes collapse to the pause point and resumed time continues there");
    const auto timeline = Replay::BuildTimeline(replay);
    Require(timeline.samples.size() == 2 && timeline.samples[0].t == 1'100 && timeline.samples[1].t == 1'300,
        "Camera movement made while recording was paused is omitted");
    Require(timeline.marks.size() == 1 && timeline.marks[0].t == 1'400 && timeline.durationMicros == 2'000,
        "Highlights and duration use the collapsed playback clock");
}

void ReplayTimelineInterpolatesAndClamps() {
    namespace Replay = Oinkster::Replay;
    Replay::ReplayTimeline timeline;
    timeline.samples = {
        {1'000, 0.0, 64.0, 0.0, 350.0F, 10.0F, 0},
        {2'000, 10.0, 66.0, -20.0, 10.0F, 30.0F, 0},
        {3'000, 500.0, 70.0, 500.0, 0.0F, 0.0F, 1},
    };
    Require(!Replay::SampleAt(Replay::ReplayTimeline{}, 0), "An empty timeline has no sample");
    const auto before = Replay::SampleAt(timeline, 0);
    Require(before && before->x == 0.0 && before->t == 1'000, "Before the first sample must clamp to it");
    const auto after = Replay::SampleAt(timeline, 9'000);
    Require(after && after->x == 500.0, "After the last sample must clamp to it");
    const auto middle = Replay::SampleAt(timeline, 1'500);
    Require(middle.has_value(), "A time between samples must interpolate");
    RequireNear(middle->x, 5.0, 1e-9, "x must interpolate linearly");
    RequireNear(middle->y, 65.0, 1e-9, "y must interpolate linearly");
    RequireNear(middle->z, -10.0, 1e-9, "z must interpolate linearly");
    RequireNear(*middle->pitch, 20.0, 1e-4, "pitch must interpolate linearly");
    // 350 -> 10 is a 20 degree turn through 0, not 340 degrees back.
    RequireNear(*middle->yaw, 0.0, 1e-4, "yaw must take the shortest arc");
    const auto crossing = Replay::SampleAt(timeline, 2'800);
    Require(crossing && crossing->dimension == 1 && crossing->x == 500.0,
        "A dimension change must snap instead of interpolating");
    const auto points = Replay::DecimatePath(timeline, 2);
    Require(points.size() == 2 && points[0].x == 0.0F && points[1].x == 500.0F,
        "Decimation must keep the first and last points");
}

void ReplayTimelineSkipsMalformedCamera() {
    namespace Replay = Oinkster::Replay;
    Replay::ReadResult replay;
    const auto record = [](std::uint64_t t, std::string_view text) {
        return Replay::Record{Replay::RecordKind::Camera, t, std::vector<std::uint8_t>(text.begin(), text.end())};
    };
    replay.records = {
        record(10, "not json"),
        record(20, R"({"x":1,"y":2})"),
        record(30, R"({"x":"1","y":2,"z":3})"),
        record(40, R"([1,2,3])"),
        record(50, R"({"x":1e999,"y":2,"z":3})"),
        record(60, R"({"x":4,"y":5,"z":6})"),
    };
    const auto timeline = Replay::BuildTimeline(replay);
    Require(timeline.samples.size() == 1 && timeline.samples[0].x == 4.0, "Only the valid camera sample survives");
    Require(!timeline.cleanEnd && timeline.durationMicros == 60, "A partial replay lasts until its last record");
}

void ReplaySummaryReadsPartial() {
    namespace Replay = Oinkster::Replay;
    const auto root = std::filesystem::temp_directory_path() / "OinksterReplaySummaryTests";
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    Replay::Writer writer;
    std::string error;
    Require(writer.Open(root / "partial.oinkreplay", 1ULL, error), "Replay writer must open");
    AppendReplayText(writer, Replay::RecordKind::Camera, 250'000, R"({"x":0,"y":0,"z":0})");
    const auto partial = writer.PartialPath();
    const auto summary = Replay::Summarize(partial);
    Require(summary.error.empty() && !summary.cleanEnd, "A partial replay must summarize as recoverable");
    Require(summary.durationMicros == 250'000 && summary.sampleCount == 1, "Partial duration is the last timestamp");
    writer.Close();
    std::filesystem::remove_all(root, ignored);
}


void ReplayCaptureParsesHeadersAndClassifies() {
    namespace C = Oinkster::ReplayCapture;
    const auto header = [](std::initializer_list<std::uint8_t> bytes) {
        const std::vector<std::uint8_t> data(bytes);
        return C::ParseHeader(data);
    };
    const auto startGame = header({0x0B, 0xFF});
    Require(startGame && startGame->id == 11 && startGame->length == 1, "A one-byte header parses");
    const auto settings = header({0x8F, 0x01});
    Require(settings && settings->id == 143 && settings->length == 2, "A two-byte header parses");
    // id 11 with sub-client bits set (raw 0x40B) still reports id 11.
    const auto subClient = header({0x8B, 0x08});
    Require(subClient && subClient->id == 11 && subClient->raw == 0x40B, "Sub-client bits are masked off the id");
    Require(!header({}) && !header({0x80}) && !header({0xFF, 0xFF, 0xFF, 0xFF, 0x7F}),
        "Empty, truncated and oversized headers are rejected");

    C::ConnectionClassifier classifier;
    // The singleplayer server's view of the client: never captured, even if
    // a server-only id later appears on it.
    auto result = classifier.Observe(0xA0, C::PacketId::PlayerAuthInput);
    Require(result.direction == C::Direction::ClientToServer && !result.sessionStart, "Client traffic is recognised");
    result = classifier.Observe(0xA0, C::PacketId::StartGame);
    Require(result.direction == C::Direction::ClientToServer, "A connection's direction never flips");
    // A join seen from its first packet.
    result = classifier.Observe(0xB0, C::PacketId::NetworkSettings);
    Require(result.direction == C::Direction::ServerToClient && result.sessionStart && result.joinedFromStart
        && result.session == 1, "A join starts a complete session");
    result = classifier.Observe(0xB0, C::PacketId::LevelChunk);
    Require(result.direction == C::Direction::ServerToClient && !result.sessionStart && result.session == 1,
        "Later packets stay in the session");
    classifier.Forget(0xB0);
    result = classifier.Observe(0xB0, C::PacketId::NetworkSettings);
    Require(result.sessionStart && result.session == 2,
        "A disconnected connection address can be reused for a new session");
    // Injected mid-session: ambiguous ids wait, then a mid-stream id starts
    // an incomplete session.
    result = classifier.Observe(0xC0, 9);
    Require(result.direction == C::Direction::Unknown, "Ambiguous ids do not classify a connection");
    result = classifier.Observe(0xC0, C::PacketId::LevelChunk);
    Require(result.sessionStart && !result.joinedFromStart && result.session == 3, "A mid-session join is marked incomplete");
    Require(classifier.Observe(0, C::PacketId::StartGame).direction == C::Direction::Unknown, "A null key is ignored");
}

Oinkster::ReplayPacketEvent ReplayPacket(double seconds, std::uint32_t session, std::uint32_t id, bool start = false) {
    Oinkster::ReplayPacketEvent event{At(seconds), session, start, start, id, {}};
    event.bytes.push_back(static_cast<char>(id < 0x80 ? id : (id & 0x7FU) | 0x80U));
    if (id >= 0x80) event.bytes.push_back(static_cast<char>(id >> 7U));
    event.bytes += "payload";
    return event;
}

void ReplayModRecordsWorldSessions() {
    namespace Replay = Oinkster::Replay;
    using namespace Oinkster::ReplayCapture;
    using State = Oinkster::ReplayControlState;
    const auto root = std::filesystem::temp_directory_path() / "OinksterReplayModTests";
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    std::filesystem::create_directories(root, ignored);
    wchar_t previous[32768]{};
    const auto previousLength = GetEnvironmentVariableW(L"LOCALAPPDATA", previous, static_cast<DWORD>(std::size(previous)));
    // Recordings go under %LOCALAPPDATA%; point it at a scratch folder so the
    // test never touches the player's real replays.
    SetEnvironmentVariableW(L"LOCALAPPDATA", root.c_str());
    const auto replays = root / "Oinkster" / "Client" / "replays";
    const auto files = [&] {
        std::vector<std::filesystem::path> found;
        for (std::filesystem::directory_iterator it(replays, ignored), end; !ignored && it != end; it.increment(ignored)) {
            if (it->path().extension() == L".oinkreplay") found.push_back(it->path());
        }
        ignored.clear();
        std::ranges::sort(found);
        return found;
    };
    const auto sessionFiles = [&] {
        std::size_t count = 0;
        for (std::filesystem::directory_iterator it(replays / ".session", ignored), end; !ignored && it != end; it.increment(ignored)) ++count;
        ignored.clear();
        return count;
    };
    const auto newest = [&](const std::vector<std::filesystem::path>& before) {
        for (const auto& path : files()) {
            if (std::ranges::find(before, path) == before.end()) return Replay::BuildTimeline(Replay::Read(path));
        }
        throw std::runtime_error("no new replay was saved");
    };

    try {
        Catalog catalog;
        catalog.manager.SetCapability(Capability::PacketCapture, {CapabilityState::Available, {}});
        Require(catalog.manager.SetEnabled("replay-mod", true), "Replay Mod enables once packets are captured");
        auto* replay = catalog.manager.EnabledModulesOf<Oinkster::IReplayControlModule>().at(0);
        Require(!replay->ReplaySnapshot(At(0)).canStart && !replay->StartReplay(At(0)),
            "Nothing can start before a world is joined");

        // Joining captures the session in the background; nothing records
        // until Start (auto-record is off by default).
        catalog.manager.Publish(ReplayPacket(1, 1, PacketId::NetworkSettings, true));
        catalog.manager.Publish(ReplayPacket(1.1, 1, PacketId::StartGame));
        catalog.manager.Publish(ReplayPacket(1.2, 1, PacketId::LevelChunk));
        catalog.manager.Publish(Oinkster::PlayerTransform{At(1.3), {1, 64, 2}, 90.0F, 0.0F, 0});
        Require(replay->ReplaySnapshot(At(1.5)).state == State::Idle && replay->ReplaySnapshot(At(1.5)).canStart,
            "A join does not record by itself, but Start is offered");
        Require(files().empty() && sessionFiles() == 1, "The session is captured in the background only");

        // First clip: starts mid-session yet still carries the world.
        Require(replay->StartReplay(At(2)) && replay->MarkReplay(At(2.5)), "Start and Mark work");
        Require(replay->FinishReplay(At(3)), "Finish saves");
        auto first = files();
        Require(first.size() == 1, "One replay is saved");
        const auto clip = Replay::BuildTimeline(Replay::Read(first[0]));
        Require(clip.cleanEnd && clip.hasStartGame && clip.packetCount == 3, "A clip keeps the join so the world can be rebuilt");
        RequireNear(static_cast<double>(clip.clipStartMicros), 1'000'000.0, 1.0, "The clip starts where Start was pressed");
        Require(clip.marks.size() == 1 && clip.marks[0].label == "mark 1" && clip.samples.size() == 1, "Marks and camera samples are kept");
        RequireNear(static_cast<double>(Replay::Summarize(first[0]).durationMicros), 1'000'000.0, 1.0,
            "A clip's length runs from its start");

        // Start again in the same world after finishing.
        Require(replay->ReplaySnapshot(At(3.5)).canStart && replay->StartReplay(At(4)), "Another clip can start after Finish");
        catalog.manager.Publish(ReplayPacket(4.5, 1, PacketId::LevelChunk));
        Require(replay->FinishReplay(At(5)), "The second clip saves");
        const auto second = newest(first);
        Require(second.packetCount == 4 && second.marks.empty(), "A later clip has the whole session but only its own marks");

        // Leaving the world saves a running clip and drops the session capture.
        first = files();
        Require(replay->StartReplay(At(6)), "A third clip starts");
        catalog.manager.Publish(ReplayPacket(7, 1, PacketId::Disconnect));
        Require(files().size() == 3 && sessionFiles() == 0, "Disconnect saves the clip and removes the session capture");
        Require(!replay->ReplaySnapshot(At(8)).canStart, "Nothing can start after leaving");

        // Auto-record: a join starts a clip, and joining another world saves it.
        catalog.manager.SetModuleSetting("replay-mod.auto-record", "true");
        catalog.manager.Publish(ReplayPacket(10, 2, PacketId::NetworkSettings, true));
        Require(replay->ReplaySnapshot(At(10.5)).state == State::Recording, "Auto-record starts on join");
        catalog.manager.Publish(ReplayPacket(11, 3, PacketId::NetworkSettings, true));
        Require(files().size() == 4, "Joining another world saves the running clip");

        // Joined before Oinkster loaded: nothing can be recorded.
        Oinkster::ReplayPacketEvent midSession = ReplayPacket(20, 4, PacketId::LevelChunk);
        midSession.sessionStart = true;
        catalog.manager.Publish(midSession);
        Require(!replay->ReplaySnapshot(At(21)).canStart && files().size() == 5,
            "A mid-session join is not recorded (the auto clip of the previous world was saved)");
    } catch (...) {
        SetEnvironmentVariableW(L"LOCALAPPDATA", previousLength ? previous : nullptr);
        std::filesystem::remove_all(root, ignored);
        throw;
    }
    SetEnvironmentVariableW(L"LOCALAPPDATA", previousLength ? previous : nullptr);
    std::filesystem::remove_all(root, ignored);
}


void RakNetFramesAcksAndSplits() {
    namespace R = Oinkster::RakNet;
    const auto start = R::Clock::now();
    R::Writer writer;
    writer.VarU32(300);
    writer.U24LE(0x123456);
    writer.U64BE(0x0102030405060708ULL);
    R::Reader reader(writer.Data());
    Require(reader.VarU32() == 300 && reader.U24LE() == 0x123456 && reader.U64BE() == 0x0102030405060708ULL && reader.Ok(),
        "Integers round-trip");
    (void)reader.U8();
    Require(!reader.Ok(), "Reading past the end is caught");

    const auto ack = R::BuildAck(false, {5, 1, 2, 3, 9});
    Require(R::IsAckOrNak(ack) && R::ParseAck(ack) == std::vector<std::uint32_t>{1, 2, 3, 5, 9}, "ACK ranges round-trip");

    // A message larger than the MTU is split by the sender and reassembled
    // by the receiver; small messages pack together.
    R::Connection server(1400);
    R::Connection client(1400);
    std::vector<std::uint8_t> big(5000);
    for (std::size_t index = 0; index < big.size(); ++index) big[index] = static_cast<std::uint8_t>(index * 7);
    server.Send(big);
    server.Send(std::vector<std::uint8_t>{0xFE, 1, 2, 3});
    const auto datagrams = server.Flush(start);
    Require(datagrams.size() >= 4, "A large message is split across datagrams");
    std::vector<std::vector<std::uint8_t>> received;
    for (const auto& datagram : datagrams) {
        Require(datagram.size() <= 1400 - R::UdpOverhead, "Datagrams respect the MTU");
        for (auto& message : client.Receive(datagram, start)) received.push_back(std::move(message));
    }
    Require(received.size() == 2 && received[0] == big && received[1] == std::vector<std::uint8_t>{0xFE, 1, 2, 3},
        "Split messages reassemble in order");
    Require(server.BytesInFlight() > 0, "Sent data stays in flight until acknowledged");
    for (const auto& reply : client.Flush(start)) (void)server.Receive(reply, start);
    Require(server.BytesInFlight() == 0, "ACKs release in-flight data");

    // NAKed data is resent, and duplicates are dropped by the receiver.
    server.Send(std::vector<std::uint8_t>{0xFE, 9});
    const auto lost = server.Flush(start);
    Require(lost.size() == 1, "One datagram for one small message");
    const auto sequence = R::ParseDatagram(lost[0])->sequence;
    (void)server.Receive(R::BuildAck(true, {sequence}), start);
    const auto resent = server.Flush(start);
    Require(resent.size() == 1 && R::ParseDatagram(resent[0])->sequence != sequence, "A NAKed datagram is resent under a new sequence");
    Require(client.Receive(resent[0], start).size() == 1 && client.Receive(lost[0], start).empty(),
        "A late duplicate of a reliable frame is dropped");
}

// Recording fixture: the join sequence, a world, and a clip from 1 s to 3 s.
Oinkster::Replay::ReadResult PlaybackFixture() {
    namespace Replay = Oinkster::Replay;
    Replay::ReadResult replay;
    const auto packet = [](std::uint64_t t, std::vector<std::uint8_t> bytes) {
        return Replay::Record{Replay::RecordKind::IncomingPacket, t, std::move(bytes)};
    };
    replay.records = {
        {Replay::RecordKind::Metadata, 0, {'{', '}'}},
        packet(0, {0x8F, 0x01, 0xAA}),            // NetworkSettings (answered by us)
        packet(1, {0x02, 0, 0, 0, 0}),            // PlayStatus: LoginSuccess
        packet(2, {0x06, 0x11}),                  // ResourcePacksInfo
        packet(2, {0x52, 0x04, 'p', 'a', 'c', 'k', 0x33}), // ResourcePackDataInfo
        packet(2, {0x53, 0x04, 'p', 'a', 'c', 'k', 0, 0, 0, 0, 0x44}), // ResourcePackChunkData 0
        packet(3, {0x07, 0x22}),                  // ResourcePackStack
        // StartGame: unique id 5 (zigzag 10), runtime id 7, survival (zigzag 0), then the rest.
        packet(4, {0x0B, 0x0A, 0x07, 0x00, 0x42, 0x43}),
        packet(1'000'000, {0x3A, 0x01}),          // LevelChunk at the clip start
        packet(1'500'000, {0x13, 0x07, 0x99}),    // MovePlayer for the viewer: dropped
        packet(1'600'000, {0x13, 0x08, 0x99}),    // MovePlayer for another entity: kept
        packet(1'700'000, {0x05, 0x00}),          // Disconnect: dropped
        {Replay::RecordKind::Marker, 1'000'000, {'c', 'l', 'i', 'p', '-', 's', 't', 'a', 'r', 't'}},
    };
    replay.cleanEnd = true;
    replay.endTimestampMicros = 3'000'000;
    return replay;
}

void ReplayPlaybackHandshakeAndPacing() {
    namespace P = Oinkster::ReplayPlayback;
    using Bytes = std::vector<std::uint8_t>;
    const std::vector<Bytes> packets{{0x01, 0x02}, Bytes(300, 0x7F)};
    const auto batch = P::EncodeBatch(packets, true);
    Require(batch[0] == 0xFE && batch[1] == 0xFF && P::DecodeBatch(batch, true) == packets, "Batches round-trip");
    Require(!P::DecodeBatch(Bytes{0xFE, 0x00, 0x01}, true), "A zlib batch is not misread");

    std::string error;
    Require(!P::Session::Load(Oinkster::Replay::ReadResult{}, error) && !error.empty(), "A replay without a world is refused");
    auto session = P::Session::Load(PlaybackFixture(), error);
    Require(session.has_value(), "The fixture loads");
    Require(session->PacketCount() == 3 && session->LengthMicros() == 2'000'000, "Handshake and control packets are filtered out");

    const auto now = P::Clock::now();
    std::vector<Bytes> out;
    auto handled = session->OnClientPacket(Bytes{0xC1, 0x01, 0x00, 0x00, 0x08, 0x91}, now, out);  // protocol 2193
    Require(handled.accepted, "The supported protocol is accepted");
    Require(out.size() == 1 && out[0] == P::NetworkSettingsPacket() && session->CompressionEnabled(),
        "Network settings are answered and compression framing begins");
    out.clear();
    handled = session->OnClientPacket(Bytes{0x01, 0x00}, now, out);  // Login
    Require(handled.accepted, "Login is accepted");
    Require(out.size() == 2 && out[0] == Bytes{0x02, 0, 0, 0, 0} && out[1] == Bytes{0x06, 0x11}, "Login gets success and the pack info");
    out.clear();
    handled = session->OnClientPacket(Bytes{0x08, 0x01}, now, out);  // SendPacks
    Require(handled.accepted && out == std::vector<Bytes>{{0x52, 0x04, 'p', 'a', 'c', 'k', 0x33}},
        "A client missing recorded packs receives the captured pack description");
    out.clear();
    handled = session->OnClientPacket(Bytes{0x54, 0x04, 'p', 'a', 'c', 'k', 0, 0, 0, 0}, now, out);
    Require(handled.accepted && out == std::vector<Bytes>{{0x53, 0x04, 'p', 'a', 'c', 'k', 0, 0, 0, 0, 0x44}},
        "A requested resource-pack chunk is served from the recording");
    out.clear();
    handled = session->OnClientPacket(Bytes{0x08, 0x02}, now, out);  // AllPacksDownloaded
    Require(handled.accepted, "Downloaded packs are accepted");
    Require(out.size() == 1 && out[0] == Bytes{0x07, 0x22}, "The pack stack follows");
    out.clear();
    handled = session->OnClientPacket(Bytes{0x08, 0x04}, now, out);
    Require(!handled.accepted && !handled.error.empty() && session->CurrentStage() == P::Stage::WaitingForPacksCompleted,
        "The obsolete off-by-one completion status is rejected");
    handled = session->OnClientPacket(Bytes{0x08, 0x03}, now, out);  // Completed
    Require(handled.accepted, "The current completion status is accepted");
    Require(out.empty() && session->CurrentStage() == P::Stage::CatchingUp, "Completed starts the world");

    auto mismatch = P::Session::Load(PlaybackFixture(), error);
    out.clear();
    handled = mismatch->OnClientPacket(Bytes{0xC1, 0x01, 0x00, 0x00, 0x08, 0x90}, now, out);
    Require(!handled.accepted && !handled.error.empty() && out.size() == 1 && out[0][0] == 0x02,
        "A mismatched client protocol gets a PlayStatus rejection and a useful error");
    out.clear();

    session->Pump(now, 1 << 20, out);
    Require(out.size() == 2, "The world before the clip sends StartGame and the first chunk");
    Require(out[0] == Bytes{0x0B, 0x0A, 0x07, 0x0C, 0x42, 0x43}, "StartGame is set to spectator");
    Require(out[1] == Bytes{0x3A, 0x01}, "The first clip chunk is sent during catch-up");
    Require(session->CurrentStage() == P::Stage::Playing, "Playback starts at the clip");
    out.clear();
    session->Pump(now + std::chrono::milliseconds(300), 1 << 20, out);
    Require(out.empty(), "Packets wait for their time");
    session->Pump(now + std::chrono::milliseconds(700), 1 << 20, out);
    Require(out.size() == 1 && out[0] == Bytes{0x13, 0x08, 0x99}, "Other entities move on time; the viewer is never moved");
    session->SetPaused(true, now + std::chrono::milliseconds(700));
    RequireNear(static_cast<double>(session->PositionMicros(now + std::chrono::seconds(5))), 700'000.0, 1000.0, "Pause holds the position");
    session->SetPaused(false, now + std::chrono::seconds(5));
    session->SetSpeed(2.0F, now + std::chrono::seconds(5));
    RequireNear(static_cast<double>(session->PositionMicros(now + std::chrono::milliseconds(5500))), 1'700'000.0, 1000.0,
        "Speed scales playback");
    out.clear();
    session->Pump(now + std::chrono::seconds(7), 1 << 20, out);
    Require(session->CurrentStage() == P::Stage::Finished, "Playback finishes at the end of the clip");
}

void ReplayPlaybackUsesActiveRecordingTime() {
    namespace P = Oinkster::ReplayPlayback;
    namespace Replay = Oinkster::Replay;
    using Bytes = std::vector<std::uint8_t>;
    auto replay = PlaybackFixture();
    const auto marker = [](std::uint64_t t, std::string_view label) {
        return Replay::Record{Replay::RecordKind::Marker, t,
            std::vector<std::uint8_t>(label.begin(), label.end())};
    };
    replay.records.push_back(marker(1'200'000, "pause"));
    replay.records.push_back(marker(2'200'000, "resume"));
    std::string error;
    auto session = P::Session::Load(replay, error);
    Require(session.has_value() && session->LengthMicros() == 1'000'000,
        "A one-second recording pause is removed from the playable length");
    const auto now = P::Clock::now();
    session->BeginInjected(now);
    std::vector<Bytes> out;
    session->Pump(now, 1 << 20, out);
    out.clear();
    session->Pump(now + std::chrono::milliseconds(199), 1 << 20, out);
    Require(out.empty(), "A packet captured during the pause waits for the pause point");
    session->Pump(now + std::chrono::milliseconds(200), 1 << 20, out);
    Require(out == std::vector<Bytes>{{0x13, 0x08, 0x99}},
        "Paused packets catch up instantly when active recording time reaches the pause point");
}

void ReplayPlaybackRecreatesTheRecordingPlayer() {
    namespace P = Oinkster::ReplayPlayback;
    namespace Replay = Oinkster::Replay;
    using Bytes = std::vector<std::uint8_t>;
    auto replay = PlaybackFixture();
    const auto text = [](std::string_view json) { return Bytes(json.begin(), json.end()); };
    Bytes playerList{0x3F, 0x01, 0x01, 0x00};  // PlayerList as 1.26.5203 sends an add
    for (std::uint8_t byte = 1; byte <= 16; ++byte) playerList.push_back(byte);  // the recorder's UUID
    playerList.insert(playerList.end(), {0x0A, 0x03, 'B', 'o', 'b', 0x03, '1', '2', '3', 0xEE});  // unique id 5, name, XUID, skin...
    replay.records.insert(replay.records.begin() + 7, Replay::Record{Replay::RecordKind::IncomingPacket, 3, playerList});
    replay.records.push_back({Replay::RecordKind::IncomingPacket, 1'100'000, {0x2C, 0x01, 0x07}});  // Animate: recorder swings
    {
        // InventoryContent, window 0: three slots, an item (id 42, count 1) in slot 2.
        Bytes content{0x31, 0x00, 0x03};
        for (int slot = 0; slot < 3; ++slot) {
            const Bytes item = slot == 2 ? Bytes{0x2A, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00} : Bytes(8, 0x00);
            content.insert(content.end(), item.begin(), item.end());
        }
        content.insert(content.end(), {0x00, 0x00});  // container name
        content.insert(content.end(), 8, 0x00);       // storage item
        replay.records.push_back({Replay::RecordKind::IncomingPacket, 500'000, content});
    }
    replay.records.push_back({Replay::RecordKind::Camera, 900'000,
        text(R"({"x":1,"y":64,"z":2,"yaw":90,"pitch":10,"dimension":null,"slot":2})")});
    replay.records.push_back({Replay::RecordKind::Camera, 1'200'000,
        text(R"({"x":3,"y":64,"z":2,"yaw":90,"pitch":10,"dimension":null})")});

    std::string error;
    auto session = P::Session::Load(replay, error);
    Require(session.has_value(), "A replay with camera samples loads");
    const auto now = P::Clock::now();
    session->BeginInjected(now);
    std::vector<Bytes> out;
    session->Pump(now + std::chrono::seconds(1), 1 << 20, out);
    session->Pump(now + std::chrono::seconds(1), 1 << 20, out);
    const auto find = [&](std::uint8_t id) {
        return std::ranges::find_if(out, [id](const Bytes& packet) { return !packet.empty() && packet[0] == id; });
    };

    const auto list = find(0x3F);
    Require(list != out.end(), "The recorder's player-list entry is sent");
    Bytes ghostUuid;
    for (std::uint8_t byte = 1; byte <= 16; ++byte) ghostUuid.push_back(byte <= 8 ? byte ^ 0xA5 : byte);
    Require(std::equal(ghostUuid.begin(), ghostUuid.end(), list->begin() + 4), "Its UUID is the ghost's, not the viewer's");
    Require(list->size() >= 6 && Bytes(list->end() - 6, list->end()) == Bytes{0x03, 'B', 'o', 'b', 0x00, 0xEE},
        "The name and skin are kept and the XUID is cleared");
    Require(find(0x0B) != out.end(), "StartGame is still sent");

    const auto add = find(0x0C);
    Require(add != out.end() && std::equal(ghostUuid.begin(), ghostUuid.end(), add->begin() + 1)
        && (*add)[17] == 3 && (*add)[18] == 'B', "The recording player is spawned with its own name");
    Require(add > std::ranges::find(out, Bytes{0x3A, 0x01}), "It spawns at the clip start, after the world before it");
    const Bytes emptyHand(8, 0x00);
    Require(std::search(add->begin(), add->end(), emptyHand.begin(), emptyHand.end()) != add->end(),
        "AddPlayer holds air as a full 1.26.50 ItemInstance");

    // Held item: slot 2 of the recorded inventory, selected in the samples.
    const auto equip = find(0x1F);
    const Bytes sword{0x2A, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00};
    Require(equip != out.end() && std::search(equip->begin(), equip->end(), sword.begin(), sword.end()) != equip->end()
        && Bytes(equip->end() - 3, equip->end()) == Bytes{0x02, 0x02, 0x00},
        "The ghost holds the recorded item in its selected hotbar slot");
    Require(find(0x20) != out.end(), "The ghost's armor is sent");
    out.clear();
    session->Pump(now + std::chrono::milliseconds(1300), 1 << 20, out);
    const auto swing = find(0x2C);
    Require(swing != out.end() && (*swing)[1] == 0x01 && (*swing)[2] != 0x07, "The recorder's swing animates the ghost, not the viewer");
    const auto move = find(0x12);
    Require(move != out.end() && move->size() == 22, "The ghost walks the recorded camera path as servers move players");
    Require(Bytes(move->begin() + 1, move->begin() + 7) == Bytes{0x81, 0x80, 0xFC, 0xFF, 0x07, 0x01},
        "MoveActorAbsolute: the ghost's runtime id, on ground, then its eye position and angle bytes");
}

void ReplayPlaybackSeeksBothWays() {
    namespace P = Oinkster::ReplayPlayback;
    namespace Replay = Oinkster::Replay;
    using Bytes = std::vector<std::uint8_t>;
    auto replay = PlaybackFixture();
    // A mob spawned during the clip (AddActor, unique id 9 = zigzag 18).
    replay.records.insert(replay.records.begin() + 9,
        Replay::Record{Replay::RecordKind::IncomingPacket, 1'200'000, {0x0D, 0x12, 0x05}});
    std::string error;
    auto session = P::Session::Load(replay, error);
    Require(session.has_value(), "The fixture loads");
    const auto now = P::Clock::now();
    session->BeginInjected(now);
    std::vector<Bytes> out;
    session->Pump(now, 1 << 20, out);
    Require(!out.empty() && out[0][0] == 0x0B, "StartGame goes out once at the start");

    out.clear();
    Require(!session->SeekTo(1'500'000, now), "Seeking forward is not a rewind");
    session->Pump(now, 1 << 20, out);
    Require(std::ranges::find(out, Bytes{0x0D, 0x12, 0x05}) != out.end() && std::ranges::find(out, Bytes{0x13, 0x08, 0x99}) != out.end(),
        "A forward seek sends everything in between at once");

    out.clear();
    Require(session->SeekTo(0, now), "Seeking back rebuilds the world");
    session->Pump(now, 1 << 20, out);
    Require(!out.empty() && out[0] == Bytes{0x0E, 0x12}, "Entities the replay spawned are removed first");
    Require(std::ranges::none_of(out, [](const Bytes& packet) { return packet[0] == 0x0B; }), "StartGame is never sent twice");
    Require(std::ranges::find(out, Bytes{0x3A, 0x01}) != out.end(), "The terrain is replayed up to the target");
    Require(std::ranges::find(out, Bytes{0x0D, 0x12, 0x05}) == out.end(), "Nothing after the target is sent yet");
    RequireNear(static_cast<double>(session->PositionMicros(now)), 0.0, 1.0, "The position is the target");
}

void ReplayInjectorTakesOverOnlyALocalWorld() {
    namespace I = Oinkster::ReplayInjector;
    namespace P = Oinkster::ReplayPlayback;
    using Bytes = std::vector<std::uint8_t>;
    std::string error;
    auto session = P::Session::Load(PlaybackFixture(), error);
    Require(session.has_value(), "The fixture loads");
    const auto now = P::Clock::now();
    I::Controller controller;
    controller.BeginLoading();
    Require(controller.CurrentPhase() == I::Phase::Loading && !controller.Snapshot(now).ready, "Watch starts loading");
    controller.Loaded(std::move(*session));
    Require(controller.CurrentPhase() == I::Phase::WaitingForWorld, "A loaded replay waits for a world");

    Require(controller.OnServerPacket(1, 143, true, false, now) == I::Verdict::Pass
        && controller.CurrentPhase() == I::Phase::WaitingForWorld, "A connection to an external server is never taken over");
    Require(controller.OnServerPacket(2, 143, true, true, now) == I::Verdict::Pass
        && controller.CurrentPhase() == I::Phase::JoiningWorld, "The next local world is bound");
    Require(controller.OnServerPacket(2, 2, false, true, now) == I::Verdict::Pass, "The local world's handshake passes through");
    Require(!controller.Next(2, now), "Nothing is injected before the pack list");
    Require(controller.OnServerPacket(2, 6, false, true, now) == I::Verdict::Drop, "The local world's pack list is replaced");
    const auto packsInfo = controller.Next(2, now);
    Require(packsInfo && *packsInfo == Bytes{0x06, 0x11}, "by the recorded server's");
    Require(controller.OnClientPacket(Bytes{0x08, 0x01, 0x00}) == I::Verdict::Drop, "A pack download request is answered by the replay");
    const auto dataInfo = controller.Next(2, now);
    Require(dataInfo && (*dataInfo)[0] == 0x52, "with the recorded pack's data info");
    Require(controller.OnClientPacket(Bytes{0x54, 0x04, 'p', 'a', 'c', 'k', 0, 0, 0, 0}) == I::Verdict::Drop, "A chunk request is answered too");
    const auto packChunk = controller.Next(2, now);
    Require(packChunk && packChunk->back() == 0x44, "with the recorded chunk");
    Require(controller.OnClientPacket(Bytes{0x08, 0x03, 0x00}) == I::Verdict::Pass, "Other pack responses reach the local world");
    Require(controller.OnServerPacket(2, 7, false, true, now) == I::Verdict::Drop, "The local world's pack stack is replaced");
    const auto stack = controller.Next(2, now);
    Require(stack && *stack == Bytes{0x07, 0x22}, "by the recorded server's");
    Require(controller.OnServerPacket(3, 50, false, true, now) == I::Verdict::Pass, "Other connections are untouched");
    Require(controller.OnServerPacket(2, 11, false, true, now) == I::Verdict::Drop
        && controller.CurrentPhase() == I::Phase::Playing, "After the pack stack the local world's packets are replaced");
    Require(controller.OnClientPacket(Bytes{0x90, 0x01}) == I::Verdict::Drop,
        "Recorded gameplay packets never reach the throwaway local server");
    Require(!controller.Next(3, now), "Only the bound connection receives the recording");
    const auto startGame = controller.Next(2, now);
    Require(startGame && *startGame == Bytes{0x0B, 0x0A, 0x07, 0x0C, 0x42, 0x43}, "The recorded StartGame comes first, as spectator");
    const auto chunk = controller.Next(2, now);
    Require(chunk && *chunk == Bytes{0x3A, 0x01}, "The world before the clip is rebuilt");
    Require(!controller.Next(2, now + std::chrono::milliseconds(300)), "Recorded packets wait for their time");
    const auto move = controller.Next(2, now + std::chrono::milliseconds(700));
    Require(move && *move == Bytes{0x13, 0x08, 0x99}, "Playback follows the recording's timing");
    Require(controller.OnServerPacket(2, 58, false, true, now) == I::Verdict::Drop, "Later local-world packets stay dropped");

    controller.SetPaused(true, now + std::chrono::milliseconds(700));
    Require(controller.Snapshot(now + std::chrono::seconds(5)).paused, "Pause is reported");
    Require(controller.OnServerPacket(4, 143, true, true, now) == I::Verdict::Pass
        && controller.CurrentPhase() == I::Phase::Idle, "Joining another world ends the replay");
}

void ReplayRewindPutsPlacedBlocksBack() {
    namespace P = Oinkster::ReplayPlayback;
    namespace Replay = Oinkster::Replay;
    using Bytes = std::vector<std::uint8_t>;
    auto replay = PlaybackFixture();
    // UpdateBlock: sandstone (hashed id 0xA43117FB) placed at (-424, 182, 39), layer 0.
    const Bytes position{0xCF, 0x06, 0xB6, 0x01, 0x4E};
    Bytes place{0x15};
    place.insert(place.end(), position.begin(), position.end());
    place.insert(place.end(), {0xFB, 0xAF, 0xC4, 0xA1, 0x0A, 0x03, 0x00});
    replay.records.insert(replay.records.begin() + 9, Replay::Record{Replay::RecordKind::IncomingPacket, 1'200'000, place});
    std::string error;
    auto session = P::Session::Load(replay, error);
    Require(session.has_value(), "The fixture loads");
    const auto now = P::Clock::now();
    session->BeginInjected(now);
    std::vector<Bytes> out;
    session->Pump(now, 1 << 20, out);
    Require(!session->SeekTo(1'500'000, now), "Seeking forward is not a rewind");
    out.clear();
    session->Pump(now, 1 << 20, out);
    Require(std::ranges::find(out, place) != out.end(), "The block is placed when the replay reaches it");

    out.clear();
    Require(session->SeekTo(0, now), "Seeking back rebuilds the world");
    session->Pump(now, 1 << 20, out);
    Bytes air{0x15};
    air.insert(air.end(), position.begin(), position.end());
    air.insert(air.end(), {0xA0, 0x82, 0xD1, 0xDF, 0x0D, 0x03, 0x00});
    Require(!out.empty() && out[0] == air, "A rewind first puts the placed block back to air (hashed id 0xDBF44120)");
    Require(std::ranges::find(out, place) == out.end(), "It is not placed again before the replay reaches it");
}

void ReplayPlayersAreTrackedAndPicked() {
    namespace I = Oinkster::ReplayInjector;
    using Bytes = std::vector<std::uint8_t>;
    const auto f32 = [](Bytes& bytes, float value) {
        const auto bits = std::bit_cast<std::uint32_t>(value);
        for (unsigned shift = 0; shift < 32; shift += 8) bytes.push_back(static_cast<std::uint8_t>(bits >> shift));
    };
    // AddPlayer "Bob", runtime id 5, feet at (1, 64, 2), facing south (yaw 0).
    Bytes add{0x0C};
    add.insert(add.end(), 16, 0x11);
    add.insert(add.end(), {0x03, 'B', 'o', 'b', 0x05, 0x00});
    for (const float value : {1.0F, 64.0F, 2.0F, 0.0F, 0.0F, 0.0F, 10.0F, 0.0F, 0.0F}) f32(add, value);
    add.insert(add.end(), 8, 0x00);
    // MovePlayer for Bob: eye at (3, 65.62, 2), facing west (yaw 90).
    Bytes move{0x13, 0x05};
    for (const float value : {3.0F, 65.62F, 2.0F, 0.0F, 90.0F, 90.0F}) f32(move, value);
    move.insert(move.end(), {0x00, 0x01, 0x00, 0x00, 0x00});

    I::PlayerTracker tracker;
    const auto now = I::Clock::now();
    tracker.Observe(Bytes{0x13, 0x09, 0x00}, now);
    Require(tracker.Players().empty(), "A MovePlayer for an unknown player adds nobody");
    // A cosmetic the server spawns as a player (never on the player list).
    Bytes cosmetic = add;
    std::fill_n(cosmetic.begin() + 1, 16, std::uint8_t{0x22});
    cosmetic[21] = 0x06;  // runtime id 6
    tracker.Observe(cosmetic, now);
    Require(tracker.Players().empty(), "A spawned player who is not on the player list is not offered");
    Bytes list{0x3F, 0x01, 0x01, 0x00};
    list.insert(list.end(), 16, std::uint8_t{0x11});
    list.insert(list.end(), {0x0A, 0x03, 'B', 'o', 'b', 0x02, '4', '2'});
    // The NPC has a list entry too (for its model), with XUID "0".
    list.insert(list.end(), 16, std::uint8_t{0x22});
    list.insert(list.end(), {0x0C, 0x03, 'H', 'a', 't', 0x01, '0'});
    tracker.Observe(list, now);
    tracker.Observe(add, now);
    auto players = tracker.Players();
    Require(players.size() == 1 && players[0].name == "Bob" && players[0].runtimeId == 5, "AddPlayer is tracked by name and id");
    RequireNear(players[0].eye.y, 65.62, 0.001, "AddPlayer's feet position becomes eye height");

    tracker.Observe(move, now + std::chrono::milliseconds(100));
    const auto halfway = tracker.PoseAt(5, now + std::chrono::milliseconds(150));
    Require(halfway.has_value(), "A tracked player has a pose");
    RequireNear(halfway->eye.x, 2.0, 0.01, "Between updates the pose eases from the old position to the new one");
    RequireNear(halfway->yaw, 45.0, 0.1, "and turns halfway");
    RequireNear(tracker.PoseAt(5, now + std::chrono::seconds(1))->eye.x, 3.0, 0.001, "and then arrives");

    players = tracker.Players();
    const Oinkster::Vec3 viewer{3.0, 65.62, -6.0};
    Require(I::PlayerAlongRay(players, viewer, 0.0F, 0.0F).has_value(), "Looking south at Bob picks him");
    Require(!I::PlayerAlongRay(players, viewer, 180.0F, 0.0F), "Looking away picks nobody");
    Require(!I::PlayerAlongRay(players, viewer, 0.0F, -80.0F), "Looking up past him picks nobody");

    const auto behind = I::BehindView(*tracker.PoseAt(5, now + std::chrono::seconds(1)));
    RequireNear(behind.eye.x, 6.5, 0.01, "A teleport lands behind a player facing west (+X of them)");

    // Servers move other players with MoveActorAbsolute: flags, eye position,
    // then pitch, body yaw and head yaw in 1/256ths of a turn.
    Bytes absolute{0x12, 0x05, 0x00};
    for (const float value : {10.0F, 70.62F, 2.0F}) f32(absolute, value);
    absolute.insert(absolute.end(), {0x20, 0x00, 0x80});
    tracker.Observe(absolute, now + std::chrono::seconds(2));
    auto pose = tracker.PoseAt(5, now + std::chrono::seconds(5));
    RequireNear(pose->eye.x, 10.0, 0.001, "MoveActorAbsolute moves a tracked player");
    RequireNear(pose->eye.y, 70.62, 0.001, "at eye height");
    RequireNear(pose->yaw, 180.0, 0.01, "and turns the view to where their head points, not their body");
    RequireNear(pose->pitch, 45.0, 0.01, "and tilts it");
    // MoveActorDelta carries only what changed: here z and head yaw.
    Bytes delta{0x6F, 0x05, 0x24, 0x00};
    f32(delta, 8.0F);
    delta.push_back(0x40);
    tracker.Observe(delta, now + std::chrono::seconds(6));
    pose = tracker.PoseAt(5, now + std::chrono::seconds(10));
    RequireNear(pose->eye.x, 10.0, 0.001, "MoveActorDelta keeps the fields it does not carry");
    RequireNear(pose->eye.z, 8.0, 0.001, "and updates the ones it does");
    RequireNear(pose->yaw, 90.0, 0.01, "including head yaw");
    RequireNear(pose->pitch, 45.0, 0.01, "while pitch stays");
    Bytes mob{0x12, 0x63, 0x00};
    for (const float value : {0.0F, 0.0F, 0.0F}) f32(mob, value);
    mob.insert(mob.end(), {0x00, 0x00, 0x00});
    tracker.Observe(mob, now + std::chrono::seconds(6));
    Require(tracker.Players().size() == 1 && !tracker.PoseAt(0x63, now), "A moving mob is not tracked as a player");
}

void ReplayExitLeavesTheWorld() {
    namespace I = Oinkster::ReplayInjector;
    namespace P = Oinkster::ReplayPlayback;
    using Bytes = std::vector<std::uint8_t>;
    std::string error;
    auto session = P::Session::Load(PlaybackFixture(), error);
    Require(session.has_value(), "The fixture loads");
    const auto now = P::Clock::now();
    I::Controller controller;
    controller.BeginLoading();
    controller.Loaded(std::move(*session));
    (void)controller.OnServerPacket(2, 143, true, true, now);
    (void)controller.OnServerPacket(2, 7, false, true, now);
    (void)controller.OnServerPacket(2, 11, false, true, now);
    Require(controller.CurrentPhase() == I::Phase::Playing, "The replay plays");
    controller.Spectate(12345, now);
    (void)controller.Next(2, now + std::chrono::milliseconds(20));
    Require(!controller.Spectating(), "Following a player who is not there stops");

    controller.Exit();
    const auto disconnect = controller.Next(2, now);
    Require(disconnect && *disconnect == Bytes{0x05, 0x00, 0x01}, "Exit hands the game a Disconnect without a message");
    Require(controller.CurrentPhase() == I::Phase::Idle, "and ends the replay");
    Require(!controller.Next(2, now), "Nothing follows it");
}

void ReplayWorldCleanupFindsOnlyItsOwnWorld() {
    namespace W = Oinkster::ReplayWorld;
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() / std::format("oinkster-world-test-{}", GetCurrentProcessId());
    fs::remove_all(root);
    const auto world = [&](const fs::path& base, std::string_view name) {
        const auto folder = base / "games" / "com.mojang" / "minecraftWorlds" / W::WorldFolderName;
        fs::create_directories(folder / "db");
        std::ofstream(folder / "levelname.txt", std::ios::binary) << name;
        return folder;
    };
    const auto replayWorld = world(root / "Users" / "123", W::WorldDisplayName);
    (void)world(root / "Users" / "456", "My Survival World");
    const auto uwpWorld = world(root, std::string(W::WorldDisplayName) + "\r\n");
    auto found = W::FindReplayWorlds(root);
    std::ranges::sort(found);
    auto expected = std::vector<fs::path>{replayWorld, uwpWorld};
    std::ranges::sort(expected);
    Require(found == expected, "Only folders named like the replay world and titled Oinkster Replay are found");
    Require(W::FindReplayWorlds(root / "missing").empty(), "A missing data root finds nothing");
    fs::remove_all(root);
}

void ReplayViewHidesTheRecordingPlayer() {
    namespace I = Oinkster::ReplayInjector;
    namespace P = Oinkster::ReplayPlayback;
    namespace Replay = Oinkster::Replay;
    using Bytes = std::vector<std::uint8_t>;
    auto replay = PlaybackFixture();
    const auto text = [](std::string_view json) { return Bytes(json.begin(), json.end()); };
    // InventoryContent, window 0: one item (id 42) in slot 0, so the recording player holds it.
    Bytes content{0x31, 0x00, 0x01, 0x2A, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    content.insert(content.end(), 8, 0x00);
    replay.records.push_back({Replay::RecordKind::IncomingPacket, 500'000, content});
    replay.records.push_back({Replay::RecordKind::Camera, 900'000, text(R"({"x":1,"y":64,"z":2,"yaw":90,"pitch":10})")});
    replay.records.push_back({Replay::RecordKind::Camera, 1'200'000, text(R"({"x":3,"y":64,"z":2,"yaw":90,"pitch":10})")});
    std::string error;
    auto session = P::Session::Load(replay, error);
    Require(session.has_value() && session->HasGhost(), "A replay with camera samples re-creates the recording player");
    const auto now = P::Clock::now();
    I::Controller controller;
    controller.BeginLoading();
    controller.Loaded(std::move(*session));
    (void)controller.OnServerPacket(2, 143, true, true, now);
    (void)controller.OnServerPacket(2, 7, false, true, now);
    (void)controller.OnServerPacket(2, 11, false, true, now);
    const auto later = now + std::chrono::milliseconds(400);
    while (controller.Next(2, later)) {}
    const auto players = controller.Players();
    Require(players.size() == 1 && players[0].recorder, "The recording player is tracked");

    controller.Spectate(P::RecorderRuntimeId, later);
    const auto remove = controller.Next(2, later);
    Require(remove && !remove->empty() && (*remove)[0] == 0x0E, "Viewing the recording player despawns them first");
    const auto view = controller.Next(2, later + std::chrono::milliseconds(20));
    Require(view && (*view)[0] == 0x13, "then the viewer's camera is moved to their eyes");
    {
        Oinkster::RakNet::Reader reader(std::span(*view).subspan(1));
        (void)reader.VarU64();
        float fields[5]{};
        for (auto& field : fields) {
            const auto bytes = reader.Bytes(4);
            Require(bytes.size() == 4, "The view carries a position and angles");
            field = std::bit_cast<float>(static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8U)
                | (static_cast<std::uint32_t>(bytes[2]) << 16U) | (static_cast<std::uint32_t>(bytes[3]) << 24U));
        }
        Require(fields[0] >= 1.0F && fields[0] <= 3.0F, "following the recorded camera path");
        RequireNear(fields[1], 65.62, 0.001, "at the recorded eye height");
        RequireNear(fields[3], 10.0, 0.001, "with the recorded pitch");
        RequireNear(fields[4], 90.0, 0.001, "and yaw");
    }
    Require(controller.Spectating() == P::RecorderRuntimeId, "and keeps following");

    controller.Spectate(std::nullopt, later);
    const auto spawn = controller.Next(2, later);
    Require(spawn && (*spawn)[0] == 0x0C, "Stopping spawns the recording player again");
    bool equipped = false;
    while (const auto next = controller.Next(2, later)) equipped |= (*next)[0] == 0x1F || (*next)[0] == 0x20;
    Require(equipped, "with what they hold and wear");
}

void ReplayRecorderFoundUnderAnotherListId() {
    namespace P = Oinkster::ReplayPlayback;
    namespace Replay = Oinkster::Replay;
    using Bytes = std::vector<std::uint8_t>;
    auto replay = PlaybackFixture();
    const auto text = [](std::string_view json) { return Bytes(json.begin(), json.end()); };
    // As on The Hive: StartGame says unique id 5, the list files the player under 17.
    Bytes playerList{0x3F, 0x01, 0x01, 0x00};
    for (std::uint8_t byte = 1; byte <= 16; ++byte) playerList.push_back(byte);
    playerList.insert(playerList.end(), {0x22, 0x03, 'B', 'o', 'b', 0x03, '1', '2', '3', 0xEE});
    replay.records.insert(replay.records.begin() + 7, Replay::Record{Replay::RecordKind::IncomingPacket, 3, playerList});
    replay.records.push_back({Replay::RecordKind::Camera, 900'000, text(R"({"x":1,"y":64,"z":2,"yaw":90,"pitch":10})")});
    std::string error;
    auto session = P::Session::Load(replay, error);
    Require(session.has_value() && session->HasGhost() && session->GhostHasSkin(),
        "The recording player is found by the first list entry nobody spawns");
    const auto now = P::Clock::now();
    session->BeginInjected(now);
    std::vector<Bytes> out;
    session->Pump(now + std::chrono::seconds(1), 1 << 20, out);
    session->Pump(now + std::chrono::seconds(1), 1 << 20, out);
    const auto list = std::ranges::find_if(out, [](const Bytes& packet) { return packet[0] == 0x3F; });
    Bytes ghostUuid;
    for (std::uint8_t byte = 1; byte <= 16; ++byte) ghostUuid.push_back(byte <= 8 ? byte ^ 0xA5 : byte);
    Require(list != out.end() && std::equal(ghostUuid.begin(), ghostUuid.end(), list->begin() + 4),
        "Its list entry is re-keyed to the ghost");
    const auto add = std::ranges::find_if(out, [](const Bytes& packet) { return packet[0] == 0x0C; });
    Require(add != out.end() && (*add)[17] == 3 && (*add)[18] == 'B', "and the ghost carries its name");
}

void ReplayRecorderFoundInLargeServerList() {
    namespace P = Oinkster::ReplayPlayback;
    namespace Replay = Oinkster::Replay;
    using Bytes = std::vector<std::uint8_t>;
    auto replay = PlaybackFixture();
    const auto text = [](std::string_view json) { return Bytes(json.begin(), json.end()); };
    // A 40-entry list (28 01 00). Its first UUID ends BE 87, so read one byte
    // early (01 00 as the UUID's start) BE 87 EC 42 is a valid unique id and
    // the name and XUID still follow: that entry must not be the one chosen.
    Bytes uuid;
    for (std::uint8_t byte = 1; byte <= 14; ++byte) uuid.push_back(byte);
    uuid.insert(uuid.end(), {0xBE, 0x87});
    Bytes playerList{0x3F, 0x28, 0x01, 0x00};
    playerList.insert(playerList.end(), uuid.begin(), uuid.end());
    playerList.insert(playerList.end(), {0xEC, 0x42, 0x03, 'B', 'o', 'b', 0x03, '1', '2', '3', 0xEE});
    replay.records.insert(replay.records.begin() + 7, Replay::Record{Replay::RecordKind::IncomingPacket, 3, playerList});
    replay.records.push_back({Replay::RecordKind::Camera, 900'000, text(R"({"x":1,"y":64,"z":2,"yaw":90,"pitch":10})")});
    std::string error;
    auto session = P::Session::Load(replay, error);
    Require(session.has_value() && session->GhostHasSkin(), "The recording player is found in a large list");
    const auto now = P::Clock::now();
    session->BeginInjected(now);
    std::vector<Bytes> out;
    session->Pump(now + std::chrono::seconds(1), 1 << 20, out);
    session->Pump(now + std::chrono::seconds(1), 1 << 20, out);
    const auto list = std::ranges::find_if(out, [](const Bytes& packet) { return packet[0] == 0x3F; });
    Require(list != out.end() && list->size() > 20, "The list is played");
    Require((*list)[1] == 0x28 && (*list)[2] == 0x01 && (*list)[3] == 0x00, "Its header is left alone");
    Bytes ghostUuid = uuid;
    for (std::size_t index = 0; index < 8; ++index) ghostUuid[index] ^= 0xA5;
    Require(std::equal(ghostUuid.begin(), ghostUuid.end(), list->begin() + 4), "and the entry right after it is re-keyed");
}

void ReplayOldHeightChunksAreModernized() {
    namespace P = Oinkster::ReplayPlayback;
    namespace Replay = Oinkster::Replay;
    using Bytes = std::vector<std::uint8_t>;
    // One version-8 sub-chunk: 1 bit per block, a named palette (stone), then
    // 16 biome sections (one plains section, 15 "same as below") and no
    // border blocks - a 0-255 world's full chunk.
    Bytes storage{0x02};
    storage.insert(storage.end(), 512, 0x00);
    storage.insert(storage.end(), {0x02, 0x0A, 0x00, 0x08, 0x04, 'n', 'a', 'm', 'e', 0x05, 's', 't', 'o', 'n', 'e', 0x00});
    Bytes payload{0x08, 0x01};
    payload.insert(payload.end(), storage.begin(), storage.end());
    payload.insert(payload.end(), {0x00, 0x01});
    payload.insert(payload.end(), 15, 0xFF);
    payload.push_back(0x00);
    Oinkster::RakNet::Writer chunk;
    chunk.VarU32(0x3A);
    for (const std::uint8_t field : Bytes{0x04, 0x06, 0x00, 0x01, 0x00, 0x00, 0x00}) chunk.U8(field);
    chunk.VarU32(static_cast<std::uint32_t>(payload.size()));
    chunk.Bytes(payload);
    auto replay = PlaybackFixture();
    replay.records.insert(replay.records.begin() + 9, Replay::Record{Replay::RecordKind::IncomingPacket, 1'200'000, chunk.Take()});
    std::string error;
    auto session = P::Session::Load(replay, error);
    Require(session.has_value() && session->ModernizedChunks() == 1, "An old-height chunk is rewritten; modern ones are not");
    const auto now = P::Clock::now();
    session->BeginInjected(now);
    std::vector<Bytes> out;
    for (int step = 0; step <= 40; ++step) session->Pump(now + std::chrono::milliseconds(100 * step), 1 << 20, out);
    const auto found = std::ranges::find_if(out, [](const Bytes& packet) { return packet.size() > 500 && packet[0] == 0x3A; });
    Require(found != out.end(), "The chunk is played");
    Bytes expected{0x09, 0x01, 0x00};
    expected.insert(expected.end(), storage.begin(), storage.end());
    expected.insert(expected.end(), {0x00, 0x01, 0xFF, 0xFF, 0xFF, 0x00, 0x01});
    expected.insert(expected.end(), 15 + 4, 0xFF);
    expected.push_back(0x00);
    Oinkster::RakNet::Reader reader(std::span(*found).subspan(8));
    const auto length = reader.VarU32();
    const auto rewritten = reader.Rest();
    Require(length == rewritten.size() && Bytes(rewritten.begin(), rewritten.end()) == expected,
        "Sub-chunks become version 9 at their absolute height and the biomes cover -64 to 320");
}

void ServerBlocksBecomeABehaviorPack() {
    namespace B = Oinkster::ReplayBlockPack;
    using Bytes = std::vector<std::uint8_t>;
    Bytes packet{0x0B, 0x05, 0x02, 0x99, 0x10};  // StartGame and some unrelated fields
    const auto text = [&](std::string_view value) {
        packet.push_back(static_cast<std::uint8_t>(value.size()));
        packet.insert(packet.end(), value.begin(), value.end());
    };
    const auto field = [&](std::uint8_t tag, std::string_view name) {
        packet.push_back(tag);
        text(name);
    };
    // hive:shutter - a model, a placement event (dropped), one state that
    // turns it, in the network form (query.block_property, quarter turns).
    text("hive:shutter");
    packet.insert(packet.end(), {0x0A, 0x00});
    field(10, "components");
    field(10, "minecraft:geometry");
    field(8, "identifier");
    text("geometry.hive.shutter");
    packet.push_back(0);
    field(10, "minecraft:on_player_placing");
    field(8, "event");
    text("turn");
    packet.push_back(0);
    packet.push_back(0);
    field(9, "properties");
    packet.insert(packet.end(), {0x0A, 0x02});
    field(8, "name");
    text("custom:facing");
    field(9, "enum");
    packet.insert(packet.end(), {0x03, 0x04, 0x00, 0x02});
    packet.push_back(0);
    field(9, "permutations");
    packet.insert(packet.end(), {0x0A, 0x02});
    field(8, "condition");
    text("query.block_property('custom:facing') == 1");
    field(10, "components");
    field(10, "minecraft:transformation");
    field(3, "RY");
    packet.push_back(0x02);
    packet.push_back(0);
    packet.push_back(0);
    packet.push_back(0);
    field(10, "menu_category");
    field(8, "category");
    text("construction");
    field(8, "group");
    text("");
    packet.push_back(0);
    packet.push_back(0);
    // hive:plain - a full cube textured by the resource pack.
    text("hive:plain");
    packet.insert(packet.end(), {0x0A, 0x00});
    field(10, "components");
    field(10, "minecraft:light_emission");
    field(3, "emission");
    packet.push_back(24);  // zigzag 12
    packet.push_back(0);
    packet.push_back(0);
    packet.push_back(0);
    packet.insert(packet.end(), {0x00, 0x01, 0x07});  // the fields after the list

    const auto blocks = B::FindCustomBlocks(packet);
    Require(blocks.size() == 2 && blocks[0].name == "hive:shutter" && blocks[1].name == "hive:plain",
        "The server's block definitions are found in StartGame, in order");
    const auto shutter = B::BlockJson(blocks[0]);
    Require(Oinkster::Json::Parse(shutter).has_value(), "A block file is valid JSON");
    Require(shutter.find(R"("identifier": "hive:shutter")") != std::string::npos
        && shutter.find(R"("states": {"custom:facing": [0, 1]})") != std::string::npos,
        "It keeps the identifier and the block's states");
    Require(shutter.find(R"("minecraft:geometry": {"identifier": "geometry.hive.shutter"})") != std::string::npos,
        "and its model");
    Require(shutter.find("query.block_state('custom:facing') == 1") != std::string::npos
        && shutter.find(R"("rotation": [0, 90, 0])") != std::string::npos,
        "Permutations read states the current way and rotate in degrees");
    Require(shutter.find("on_player_placing") == std::string::npos, "Events are left out");
    Require(B::BlockJson(blocks[1]).find(R"("minecraft:light_emission": 12)") != std::string::npos, "Light is kept");
    const auto pack = B::BuildPack(blocks);
    Require(pack.size() == 3 && pack[0].path == "manifest.json" && Oinkster::Json::Parse(pack[0].text).has_value()
        && Oinkster::Json::Parse(B::WorldPacksJson()).has_value(), "The pack is a manifest plus one file per block");
    Require(B::FindCustomBlocks(Bytes{0x0B, 0x00, 0x01}).empty(), "A StartGame without custom blocks has none");
}

void CameraKeyframesMakeASmoothPath() {
    namespace C = Oinkster::ReplayCameraPath;
    C::Path path;
    Require(!path.Evaluate(0), "No keyframes, no camera");
    path.Add({2'000'000, {10.0, 70.0, 0.0}, 170.0F, 10.0F});
    path.Add({0, {0.0, 70.0, 0.0}, 0.0F, 0.0F});
    path.Add({4'000'000, {20.0, 80.0, 10.0}, -170.0F, -20.0F});
    Require(path.Size() == 3 && path.Keyframes()[0].micros == 0 && path.Keyframes()[2].micros == 4'000'000, "Keyframes keep time order");
    RequireNear(path.Evaluate(2'000'000)->eye.x, 10.0, 1e-9, "The path passes through each keyframe");
    RequireNear(path.Evaluate(9'000'000)->eye.y, 80.0, 1e-9, "and holds the last one after it");
    const auto between = *path.Evaluate(1'000'000);
    Require(between.eye.x > 0.0 && between.eye.x < 10.0, "Between keyframes it moves from one to the next");
    // 170 to -170 is a 20-degree turn across 180, not 340 the long way.
    const float turning = path.Evaluate(3'000'000)->yaw;
    Require(std::abs(turning) > 150.0F, "The camera turns the short way round (near 180, not through 0)");
    Require(path.Add({2'050'000, {11.0, 70.0, 0.0}, 0.0F, 0.0F}) == 1 && path.Size() == 3, "Adding at the same moment replaces it");
    Require(!path.Retime(0, 2'020'000) && path.Retime(0, 3'000'000) == 1, "A keyframe moves in time, but not onto another");

    const auto loaded = C::Path::FromJson(path.ToJson());
    Require(loaded && *loaded == path, "Keyframes save and load");
    Require(!C::Path::FromJson("not json") && C::Path::FromJson(R"({"keyframes": []})")->Empty(), "A bad file loads nothing");
}

void CameraKeyframesEaseAndCut() {
    namespace C = Oinkster::ReplayCameraPath;
    C::Path path;
    path.Add({0, {0.0, 64.0, 0.0}, 0.0F, 0.0F});
    path.Add({1'000'000, {10.0, 64.0, 0.0}, 0.0F, 0.0F});
    const double smooth = path.Evaluate(250'000)->eye.x;
    path.SetEase(0, C::Ease::EaseInOut);
    const double eased = path.Evaluate(250'000)->eye.x;
    Require(eased < smooth && eased > 0.0, "Ease in/out starts slower than the even glide");
    RequireNear(path.Evaluate(500'000)->eye.x, 5.0, 1e-9, "and is halfway at the halfway point");
    path.SetEase(0, C::Ease::Cut);
    RequireNear(path.Evaluate(999'000)->eye.x, 0.0, 1e-9, "A cut holds the shot until the next keyframe");
    RequireNear(path.Evaluate(1'000'000)->eye.x, 10.0, 1e-9, "then jumps to it");
    const auto loaded = C::Path::FromJson(path.ToJson());
    Require(loaded && loaded->Keyframes()[0].ease == C::Ease::Cut && *loaded == path, "Easing is saved with the keyframes");
    path.Add({20'000, {3.0, 64.0, 0.0}, 0.0F, 0.0F});
    Require(path.Keyframes()[0].ease == C::Ease::Cut, "Re-pinning a keyframe keeps its easing");
}

void VideosEncodeToMp4() {
    namespace V = Oinkster::VideoRecorder;
    const auto file = std::filesystem::temp_directory_path() / std::format("oinkster-video-test-{}.mp4", GetCurrentProcessId());
    std::filesystem::remove(file);
    std::string error;
    Require(V::WriteTestVideo(file, 322, 180, 30, error), ("An H.264 MP4 is written (" + error + ")").c_str());
    std::error_code ignored;
    Require(std::filesystem::file_size(file, ignored) > 1000, "and has video in it");
    std::ifstream video(file, std::ios::binary);
    std::string head(12, '\0');
    video.read(head.data(), 12);
    Require(head.substr(4, 4) == "ftyp", "It is an MP4 file");
    video.close();
    std::filesystem::remove(file, ignored);
    Require(!V::Recording(), "Nothing is recording");
}

void ReplayCameraFliesAlongThePath() {
    namespace I = Oinkster::ReplayInjector;
    namespace P = Oinkster::ReplayPlayback;
    std::string error;
    auto session = P::Session::Load(PlaybackFixture(), error);
    Require(session.has_value(), "The fixture loads");
    const auto now = P::Clock::now();
    I::Controller controller;
    controller.BeginLoading();
    controller.Loaded(std::move(*session));
    (void)controller.OnServerPacket(2, 143, true, true, now);
    (void)controller.OnServerPacket(2, 7, false, true, now);
    (void)controller.OnServerPacket(2, 11, false, true, now);
    controller.PlayCameraPath(true, now);
    Require(!controller.PlayingCameraPath(), "An empty path cannot play");
    Oinkster::ReplayCameraPath::Path path;
    path.Add({0, {1.0, 65.0, 2.0}, 90.0F, 30.0F});
    path.Add({60'000'000, {5.0, 65.0, 2.0}, 90.0F, 30.0F});
    controller.SetCameraPath(path);
    controller.PlayCameraPath(true, now);
    const auto view = controller.Next(2, now + std::chrono::milliseconds(20));
    Require(view && (*view)[0] == 0x13, "Playing the path moves the viewer's camera");
    Oinkster::RakNet::Reader reader(std::span(*view).subspan(1));
    (void)reader.VarU64();
    const auto pitchBytes = reader.Bytes(16).subspan(12, 4);
    const float pitch = std::bit_cast<float>(static_cast<std::uint32_t>(pitchBytes[0]) | (static_cast<std::uint32_t>(pitchBytes[1]) << 8U)
        | (static_cast<std::uint32_t>(pitchBytes[2]) << 16U) | (static_cast<std::uint32_t>(pitchBytes[3]) << 24U));
    RequireNear(pitch, 30.0, 0.001, "with the keyframes' look");
    controller.Spectate(12345, now);
    Require(!controller.PlayingCameraPath(), "Following a player stops the path");
    Require(controller.Snapshot(now).cameraKeyframes == 2, "The status counts the keyframes");
}

void ServerNameTagsAreCleanedForDisplay() {
    namespace I = Oinkster::ReplayInjector;
    Require(I::CleanPlayerName("\xC2\xA7iDeathBlocko\xC2\xA7" "8 [\xC2\xA7iU\xC2\xA7" "8]\n\xEE\x86\x8F \xC2\xA7p57.4K Hiders Killed")
        == "DeathBlocko [U]", "Colour codes, the second line and icon glyphs are dropped");
    Require(I::CleanPlayerName("\xC2\xA7" "7BunkerRoom55096") == "BunkerRoom55096", "A plain coloured name keeps its text");
    Require(I::CleanPlayerName("Caf\xC3\xA9 Steve") == "Caf\xC3\xA9 Steve", "Ordinary Unicode is kept");
    Require(I::CleanPlayerName("\xC2\xA7r\n") == "Player", "A name with nothing left falls back");
}


void NullMovementKeepsOnlyTheNewestKey() {
    M::NullMovementFilter filter;
    filter.Configure({{{'A', 'D'}, {}}});
    std::array<bool, 256> held{};
    std::vector<Oinkster::KeyMessage> out;
    const auto send = [&](std::uint32_t key, bool pressed, bool repeated = false) {
        held[key] = pressed;
        out.clear();
        return filter.Transform(key, pressed, repeated, held, out);
    };
    Require(!send('W', true), "Keys off a configured axis pass through untouched");
    Require(send('A', true) && out.size() == 1 && out[0].key == 'A' && out[0].pressed, "The first key goes down");
    Require(send('D', true) && out.size() == 2 && out[0].key == 'A' && !out[0].pressed && out[1].key == 'D' && out[1].pressed,
        "Pressing the opposite key releases the older one first");
    Require(send('A', true, true) && out.empty(), "Auto-repeat of the overridden key is swallowed");
    Require(send('D', true, true) && out.size() == 1 && out[0].key == 'D', "Auto-repeat of the active key continues");
    Require(send('D', false) && out.size() == 2 && out[0].key == 'D' && !out[0].pressed && out[1].key == 'A' && out[1].pressed,
        "Releasing the newer key hands the axis back to the still-held older one");
    Require(send('A', false) && out.size() == 1 && out[0].key == 'A' && !out[0].pressed, "The last release goes through");
    Require(send('D', false) && out.empty(), "A release the game never saw down is not sent");
    send('A', true);
    filter.Reset();
    held['A'] = false;
    Require(send('D', true) && out.size() == 1 && out[0].key == 'D', "After a reset nothing stale is released");
}

void ChunkBordersProjectAroundThePlayer() {
    // Camera at the origin looking down -Z (identity rotation), 90 degree FOV.
    M::CameraView view{{8.0F, 70.0F, 8.0F}, {0.0F, 0.0F, 0.0F, 1.0F}, 1.5707964F};
    const auto line = M::ProjectSegment(view, {8.0F, 70.0F, -2.0F}, {8.0F, 71.0F, -2.0F}, 200.0F, 100.0F, 0xFFFFFFFFU, 1.0F);
    Require(line.has_value(), "A segment in front of the camera projects");
    RequireNear(line->x1, 100.0, 1e-3, "Straight ahead lands on the screen centre");
    RequireNear(line->y1, 50.0, 1e-3, "Eye level lands on the vertical centre");
    Require(line->y2 < line->y1, "Up in the world is up on screen");
    Require(!M::ProjectSegment(view, {8.0F, 70.0F, 20.0F}, {8.0F, 71.0F, 20.0F}, 200.0F, 100.0F, 0, 1.0F),
        "A segment wholly behind the camera is dropped");
    const auto clipped = M::ProjectSegment(view, {8.0F, 69.0F, 20.0F}, {8.0F, 69.0F, -20.0F}, 200.0F, 100.0F, 0, 1.0F);
    Require(clipped && std::isfinite(clipped->x1) && std::isfinite(clipped->y1), "A segment crossing the camera is clipped, not dropped");
    Require(clipped->x1 >= 0.0F && clipped->x1 <= 200.0F && clipped->y1 >= 0.0F && clipped->y1 <= 100.0F,
        "Near-plane clipping also keeps projected endpoints on screen");

    M::ChunkBorderStyle style;
    style.gridSpacing = 0;
    const auto corners = M::BuildChunkBorders(style, view, {8.0F, 70.0F, 8.0F}, 200.0F, 100.0F);
    Require(!corners.lines.empty() && corners.lines.size() <= 16, "Corners only: at most the 16 lattice columns");
    std::uint32_t strongestCurrent{};
    std::uint32_t strongestNeighbor{};
    for (const auto& corner : corners.lines) {
        const auto rgb = corner.argb & 0x00FFFFFFU;
        Require(rgb == (style.cornerColor & 0x00FFFFFFU) || rgb == (style.neighborColor & 0x00FFFFFFU),
            "Corners retain the configured corner colours while distance changes opacity");
        if (rgb == (style.cornerColor & 0x00FFFFFFU)) strongestCurrent = std::max(strongestCurrent, corner.argb >> 24U);
        if (rgb == (style.neighborColor & 0x00FFFFFFU)) strongestNeighbor = std::max(strongestNeighbor, corner.argb >> 24U);
    }
    Require(strongestCurrent > strongestNeighbor, "The current chunk boundary is stronger than neighboring chunks");
    style.gridSpacing = 2;
    const auto grid = M::BuildChunkBorders(style, view, {8.0F, 70.0F, 8.0F}, 200.0F, 100.0F);
    Require(grid.lines.size() > corners.lines.size(), "The wall grid adds lines");
    float thinnest = std::numeric_limits<float>::max();
    float thickest{};
    bool sawGridColor{};
    for (const auto& segment : grid.lines) {
        thinnest = std::min(thinnest, segment.thickness);
        thickest = std::max(thickest, segment.thickness);
        sawGridColor |= (segment.argb & 0x00FFFFFFU) == (style.chunkColor & 0x00FFFFFFU);
    }
    Require(sawGridColor && thinnest < style.thickness && thickest > style.thickness,
        "Minor wall lines are thinner while section boundaries and corner pillars are emphasized");
    const auto negative = M::BuildChunkBorders(style, view, {-0.5F, 70.0F, -0.5F}, 200.0F, 100.0F);
    Require(!negative.Empty(), "Negative coordinates use floor-division and still produce stable chunk borders");
    Require(M::BuildChunkBorders(style, view, {NAN, 0.0F, 0.0F}, 200.0F, 100.0F).Empty(), "An invalid position draws nothing");
}

void PostProcessConstantsAreBounded() {
    Oinkster::PostProcessSettings settings;
    Require(settings.Identity(), "Defaults leave the frame unchanged");
    settings.saturation = 9.0F;
    settings.contrast = NAN;
    settings.tint = 0xFF336699U;
    settings.tintStrength = 0.5F;
    const auto constants = Oinkster::MakePostProcessConstants(1920.0F, 1080.0F, settings);
    RequireNear(constants.grade[0], 2.0, 1e-6, "Saturation clamps");
    RequireNear(constants.grade[1], 1.0, 1e-6, "A non-finite contrast falls back to neutral");
    RequireNear(constants.tint[0], 0x33 / 255.0, 1e-6, "Tint red comes from the ARGB colour");
    RequireNear(constants.tint[2], 0x99 / 255.0, 1e-6, "Tint blue comes from the ARGB colour");
    RequireNear(constants.extra[1], 1.0 / 1920.0, 1e-9, "Texel size follows the display");
    RequireNear(constants.projection[0], 2.0 / 1920.0, 1e-9, "The projection matches ImGui's");
    static_assert(sizeof(Oinkster::PostProcessConstants) == 28 * sizeof(float));
}

void BlueberryParityModulesCompose() {
    Catalog catalog;
    for (const auto capability : {Capability::InputFiltering, Capability::PerspectivePresentation, Capability::CameraPresentation,
        Capability::LocalPlayerPosition}) {
        catalog.manager.SetCapability(capability, {CapabilityState::Available, {}});
    }
    for (const auto* id : {"scroll-wheel-disabler", "sensitivity-multiplier", "null-movement", "modern-keyboard-handling",
        "snap-look", "chunk-borders", "post-processing", "debug-menu"}) {
        Require(catalog.manager.SetEnabled(id, true), "Parity module enables once its capabilities validate");
    }
    catalog.manager.SetModuleSetting("sensitivity-multiplier.horizontal", "9");
    catalog.manager.SetModuleSetting("sensitivity-multiplier.vertical", "0.5");
    Oinkster::MouseShaping shaping;
    for (const auto* module : catalog.manager.EnabledModulesOf<Oinkster::IMouseShapingModule>()) module->ShapeMouse(shaping, true);
    Require(shaping.blockWheel, "The wheel is blocked while gameplay owns the mouse");
    RequireNear(shaping.sensitivityX, 3.0, 1e-6, "Horizontal sensitivity clamps");
    RequireNear(shaping.sensitivityY, 0.5, 1e-6, "Vertical sensitivity applies");
    Oinkster::MouseShaping menu;
    for (const auto* module : catalog.manager.EnabledModulesOf<Oinkster::IMouseShapingModule>()) module->ShapeMouse(menu, false);
    Require(!menu.blockWheel && menu.sensitivityX == 1.0F, "Game screens keep the vanilla mouse");

    auto profile = M::CollectPresentationProfile(catalog.manager);
    Require(!profile.perspective, "Snap look is idle until its key");
    Require(profile.cameraPose && !profile.cameraPose->selection, "Chunk borders request the camera while the hidden debug overlay avoids target work");
    (void)catalog.manager.PublishInput(Oinkster::KeyboardEvent{'V', true, false});
    profile = M::CollectPresentationProfile(catalog.manager);
    Require(profile.perspective && profile.perspective->perspective == Oinkster::CameraPerspective::ThirdPersonFront,
        "Holding the key snaps to the front view");
    (void)catalog.manager.PublishInput(Oinkster::KeyboardEvent{'V', false, false});
    Require(!M::CollectPresentationProfile(catalog.manager).perspective, "Releasing it returns");

    const auto restores = catalog.manager.EnabledModulesOf<Oinkster::IHeldKeyRestoreModule>();
    Require(restores.size() == 1 && restores.front()->KeysToRestore().size() == 7, "Modern keyboard restores the seven movement keys");
    Require(catalog.manager.EnabledModulesOf<Oinkster::IKeyTransformModule>().size() == 1, "Null movement transforms keys");
    Require(catalog.manager.EnabledModulesOf<Oinkster::IPostProcessModule>().size() == 1, "Post processing grades the frame");

    const auto debugModules = catalog.manager.EnabledModulesOf<Oinkster::IDebugOverlayModule>();
    Require(debugModules.size() == 1, "The debug menu is a specialized two-sided overlay, not a movable HUD card");
    auto* debug = debugModules.front();
    Oinkster::PlayerTransform transform;
    transform.capturedAt = At(1);
    transform.position = {-0.5, 65.62, 31.5};
    transform.yawDegrees = 180.0F;
    transform.pitchDegrees = 10.0F;
    transform.dimension = 0;
    catalog.manager.Publish(transform);
    Oinkster::RenderEvent gameplay;
    gameplay.width = 1920.0F;
    gameplay.height = 1080.0F;
    gameplay.presentedAt = At(1);
    gameplay.gameplayVisible = true;
    catalog.manager.Publish(gameplay);
    Require(!debug->DebugOverlay().visible, "The enabled debug overlay starts hidden");
    (void)catalog.manager.PublishInput(Oinkster::KeyboardEvent{VK_F3, true, true});
    Require(!debug->DebugOverlay().visible, "Repeated F3 input is ignored");
    (void)catalog.manager.PublishInput(Oinkster::KeyboardEvent{VK_F3, true, false, true});
    Require(!debug->DebugOverlay().visible, "F3 captured by the UI is ignored");
    (void)catalog.manager.PublishInput(Oinkster::KeyboardEvent{VK_F3, true, false});
    const auto overlay = debug->DebugOverlay();
    Require(overlay.visible && overlay.left.size() == 2 && overlay.right.size() == 2,
        "F3 reveals exact performance/world and build/display groups");
    std::string text;
    for (const auto& group : overlay.left) for (const auto& row : group.lines) text += row.text + '\n';
    Require(text.find("Block: -1 64 31") != std::string::npos, "Block coordinates floor the feet position");
    Require(text.find("Chunk: 15 15 in -1 1") != std::string::npos, "Chunk-relative position and chunk index");
    Require(text.find("North") != std::string::npos, "Facing follows the Bedrock yaw convention");
    profile = M::CollectPresentationProfile(catalog.manager);
    Require(profile.cameraPose && profile.cameraPose->selection, "A visible debug target row requests block selection");
    gameplay.gameplayVisible = false;
    gameplay.presentedAt = At(2);
    catalog.manager.Publish(gameplay);
    Require(!debug->DebugOverlay().visible, "Menus and other non-gameplay screens hide the overlay without changing its toggle");
    gameplay.gameplayVisible = true;
    gameplay.presentedAt = At(3);
    catalog.manager.Publish(gameplay);
    const auto stale = debug->DebugOverlay();
    std::string staleText;
    for (const auto& group : stale.left) for (const auto& row : group.lines) staleText += row.text + '\n';
    Require(stale.visible && staleText.find("XYZ: --") != std::string::npos && staleText.find("Block:") == std::string::npos,
        "Returning to gameplay omits stale coordinate telemetry instead of presenting old values as current");
    Require(catalog.manager.SetEnabled("debug-menu", false), "The debug module disables cleanly");
    (void)catalog.manager.PublishInput(Oinkster::KeyboardEvent{VK_F3, true, false});
    Require(catalog.manager.SetEnabled("debug-menu", true), "The debug module can be re-enabled");
    catalog.manager.Publish(gameplay);
    Require(!debug->DebugOverlay().visible, "Disabling the module removes F3 handling and resets hidden state");
}


// A PlayerList add entry in the 1.26.5203 layout, as recorded from servers:
// marker, uuid, unique id, name, XUID, chat id, platform, the serialized
// skin through FullId, then the variable tail.
void WritePlayerListEntry(Oinkster::RakNet::Writer& writer, std::uint8_t uuidByte, std::string_view name, std::string_view xuid,
    bool personaTail) {
    const auto text = [&](std::string_view value) {
        writer.VarU32(static_cast<std::uint32_t>(value.size()));
        writer.Bytes(std::span(reinterpret_cast<const std::uint8_t*>(value.data()), value.size()));
    };
    writer.U8(1);
    writer.U8(0);
    for (int index = 0; index < 16; ++index) writer.U8(uuidByte);
    writer.VarU64(84);
    text(name);
    text(xuid);
    text("");
    writer.U32LE(8);
    text("skin-id");
    text("playfab");
    text(R"({"geometry":{"default":"geometry.humanoid.custom"}})");
    writer.U32LE(64);
    writer.U32LE(64);
    std::vector<std::uint8_t> skin(64 * 64 * 4, 0);
    // Head front base layer (8..15, 8..15) red; hat pixel at (40,8) blue.
    for (int y = 8; y < 16; ++y) for (int x = 8; x < 16; ++x) {
        const auto at = static_cast<std::size_t>((y * 64 + x) * 4);
        skin[at] = 200; skin[at + 3] = 255;
    }
    const auto hat = static_cast<std::size_t>((8 * 64 + 40) * 4);
    skin[hat + 2] = 220; skin[hat + 3] = 255;
    writer.VarU32(static_cast<std::uint32_t>(skin.size()));
    writer.Bytes(skin);
    writer.VarU32(0);   // animations
    writer.U32LE(0);    // cape width
    writer.U32LE(0);    // cape height
    text("");           // cape data
    text(R"({"minecraft:geometry":[]})");
    text("1.17.0");
    text("");           // animation data
    text("");           // cape id
    text("full-id");
    // The tail as observed (arm size, colour, flags, "true", a hash or an
    // empty string, teacher/host/sub-client, player colour).
    const std::array<std::uint8_t, 12> head{1, 0, 0, 0, 0xFF, 0, 0, 1, 0, 0, 1, 1};
    writer.Bytes(head);
    text("true");
    text(personaTail ? "+BsRJfs0IrVNuQvTU0Wm2FuOaVC3Tzyi8qJA188xJio=" : "");
    writer.U8(0); writer.U8(personaTail ? 1 : 0); writer.U8(0);
    writer.U32LE(0xFFEDEDEDU);
}

void PlayerListPacketsBuildARoster() {
    Oinkster::RakNet::Writer add;
    add.VarU32(3);
    WritePlayerListEntry(add, 0x11, "\xC2\xA7" "aAlpha", "2535400000000001", false);
    WritePlayerListEntry(add, 0x22, "Bravo", "2535400000000002", true);
    WritePlayerListEntry(add, 0x33, "Hub NPC", "0", false);
    const auto parsed = M::ParsePlayerList(add.Data());
    Require(parsed && !parsed->remove && parsed->players.size() == 3, "Every add entry is found past its skin");
    Require(parsed->players[1].name == "Bravo" && parsed->players[1].xuid == "2535400000000002", "Names and XUIDs decode");
    Require(parsed->players[0].skinWidth == 64 && parsed->players[0].skin.size() == 64 * 64 * 4, "The skin image is kept");
    const auto face = M::HeadFace(64, 64, parsed->players[0].skin);
    Require(face && (*face)[1] == 0xFFC80000U, "The head face is the base layer, ARGB");
    Require((*face)[0] == 0xFF0000DCU, "The hat layer covers the base where opaque");

    M::PlayerRoster roster;
    roster.Apply(*parsed);
    Require(roster.Players().size() == 3 && roster.Find("Alpha") != nullptr && roster.Find("alpha") != nullptr,
        "Players are found by their cleaned name, case-insensitively");
    Oinkster::RakNet::Writer remove;
    remove.VarU32(1);
    remove.U8(0);
    remove.U8(1);
    for (int index = 0; index < 16; ++index) remove.U8(0x22);
    const auto removal = M::ParsePlayerList(remove.Data());
    Require(removal && removal->remove && removal->players.size() == 1, "A removal carries uuids only");
    roster.Apply(*removal);
    Require(roster.Players().size() == 2 && roster.Find("Bravo") == nullptr, "Removed players leave the roster");

    auto truncated = add.Data();
    truncated.resize(truncated.size() / 2);
    Require(!M::ParsePlayerList(truncated), "A truncated list is rejected, not half-read");
    Require(!M::ParsePlayerList(std::vector<std::uint8_t>{1, 7, 7}), "An unknown marker is rejected");

    Catalog catalog;
    catalog.manager.SetCapability(Capability::PacketCapture, {CapabilityState::Available, {}});
    Require(catalog.manager.SetEnabled("player-list", true), "The player list enables when packet capture is available");
    Oinkster::RakNet::Writer whole;
    whole.VarU32(M::GamePacket::PlayerList);
    whole.Bytes(add.Data());
    const auto wholeBytes = whole.Data();
    catalog.manager.Publish(Oinkster::ReplayPacketEvent{At(1), 7, true, true, M::GamePacket::PlayerList,
        std::string(reinterpret_cast<const char*>(wholeBytes.data()), wholeBytes.size())});
    catalog.manager.Publish(Oinkster::ServerState{At(1), Oinkster::SessionKind::Server, L"The Hive", Oinkster::ServerNetwork::Hive});
    Oinkster::RenderEvent frame;
    frame.width = 1280.0F;
    frame.height = 720.0F;
    frame.presentedAt = At(1);
    frame.gameplayVisible = true;
    frame.keysDown = std::bitset<256>{};
    frame.keysDown->set(VK_TAB);
    catalog.manager.Publish(frame);
    const auto listModules = catalog.manager.EnabledModulesOf<Oinkster::IPlayerListModule>();
    Require(listModules.size() == 1, "The list exposes its typed renderer contract");
    const auto view = listModules.front()->PlayerList();
    Require(view.visible && view.title == "The Hive" && view.rows.size() == 2,
        "Holding Tab shows the server roster and filters NPC entries");
    Require(view.rows[0].name == "Alpha" && view.rows[1].name == "Bravo" && view.rows[0].hasFace,
        "Player rows sort alphabetically and retain decoded heads");
    catalog.manager.Publish(Oinkster::ServerState{At(2), Oinkster::SessionKind::None, L"", Oinkster::ServerNetwork::Local});
    Require(listModules.front()->PlayerList().rows.empty(), "Disconnect clears the roster instead of leaking a stale session");
}

void ChatTitleAndFormPacketsDecode() {
    // Recorded from The Hive on 1.26.5203.
    const std::vector<std::uint8_t> raw{0x00, 0x00, 0x00, 0x05, 'H', 'e', 'l', 'l', 'o', 0x00, 0x00, 0x00};
    const auto system = M::ParseText(raw);
    Require(system && system->type == 0 && system->message == "Hello" && system->source.empty(), "A raw message decodes");
    const std::vector<std::uint8_t> chat{0x00, 0x01, 0x01, 0x03, 'B', 'o', 'b', 0x02, 'h', 'i', 0x00, 0x00};
    const auto said = M::ParseText(chat);
    Require(said && said->type == 1 && said->source == "Bob" && said->message == "hi", "A chat message keeps its author");
    const std::vector<std::uint8_t> translated{0x01, 0x02, 0x02, 0x03, 'k', 'e', 'y', 0x02, 0x01, 'a', 0x01, 'b'};
    const auto keyed = M::ParseText(translated);
    Require(keyed && keyed->parameters.size() == 2 && keyed->parameters[1] == "b", "Translation parameters decode");
    Require(!M::ParseText(std::vector<std::uint8_t>{0x00, 0x05, 0x00}), "An unknown payload kind is rejected");
    const std::vector<std::uint8_t> title{0x04, 0x04, 'W', 'i', 'n', '!'};
    const auto shown = M::ParseTitle(title);
    Require(shown && shown->type == 2 && shown->text == "Win!", "Title types are zigzag varints");
    const std::vector<std::uint8_t> form{0x02, 0x02, '{', '}'};
    const auto request = M::ParseFormRequest(form);
    Require(request && request->id == 2 && request->json == "{}", "A form request carries its id and JSON");
    const std::vector<std::uint8_t> packet{0x3F, 0x01, 0x00};
    const auto split = M::SplitPacket(packet);
    Require(split && split->id == 63 && split->body.size() == 2, "The header varint gives the packet id");
}





void HiveMessagesAreRecognised() {
    // The Hive's own lines, with their colour codes.
    const auto over = M::ClassifyHiveChat("\xC2\xA7" "c\xC2\xA7" "l\xC2\xBB \xC2\xA7r\xC2\xA7" "c\xC2\xA7" "lGame OVER!");
    Require(over.kind == M::HiveEventKind::GameOver, "Game over is recognised through its formatting");
    const auto connected = M::ClassifyHiveChat("\xC2\xA7" "aYou are connected to server name SKY-DUOS12");
    Require(connected.kind == M::HiveEventKind::ConnectedServer && connected.value == "SKY-DUOS", "The game code drops the instance number");
    const auto party = M::ClassifyHiveChat("\xC2\xA7" "b\xC2\xBB \xC2\xA7" "bTurboRed80\xC2\xA7" "b wants you to join their party!");
    Require(party.kind == M::HiveEventKind::PartyInvite && party.value == "TurboRed80", "Party invites name the inviter");
    const auto friendInvite = M::ClassifyHiveChat("\xC2\xA7" "aYou received a friend invite from \xC2\xA7" "bSome Player\xC2\xA7" "a!");
    Require(friendInvite.kind == M::HiveEventKind::FriendInvite && friendInvite.value == "Some Player", "Friend invites name the sender");
    const auto team = M::ClassifyHiveChat("\xC2\xA7" "e\xC2\xBB \xC2\xA7rYou are on the \xC2\xA7" "cRed Team\xC2\xA7r!");
    Require(team.kind == M::HiveEventKind::TeamAssigned && team.value == "Red Team", "The player's team is remembered");
    const auto out = M::ClassifyHiveChat("\xC2\xA7" "cRed Team \xC2\xA7" "7has been \xC2\xA7" "cELIMINATED\xC2\xA7" "7!");
    Require(out.kind == M::HiveEventKind::TeamEliminated && out.value == "Red Team", "Team eliminations name the team");
    Require(M::ClassifyHiveChat("\xC2\xA7" "7Indigo \xC2\xBB hello").kind == M::HiveEventKind::None, "Player chat is not an event");
    Require(M::ClassifyHiveTitle("\xC2\xA7" "7You're spectating the \xC2\xA7" "as\xC2\xA7" "eh\xC2\xA7" "6o\xC2\xA7" "cw\xC2\xA7" "7!").kind == M::HiveEventKind::Died,
        "Spectating after a death counts as dying");
    const auto code = M::ClassifyHiveTitle("\xC2\xA7" "eJoin Code: \xC2\xA7" "fABC123");
    Require(code.kind == M::HiveEventKind::JoinCode && code.value == "ABC123", "Join codes are read from the title");
    Require(M::IsHiveHub("HUB-BED") && M::IsHiveHub("HUB") && !M::IsHiveHub("SKY"), "Hubs are never re-queued");

    const std::string form = R"({"type":"form","title":"\u00a7lChoose Map","content":"","buttons":[{"text":"Random\nPick any"},{"text":"\u00a7aFort\n\u00a783 votes"},{"text":"Dragon Isle\n1 vote"}]})";
    const auto buttons = M::MapVoteButtons(form);
    Require(buttons && buttons->size() == 3 && (*buttons)[1] == "Fort", "Map buttons keep their first line without formatting");
    const auto preferred = M::ParseMapList(" dragon isle, Fort ");
    Require(M::ChooseMapButton(*buttons, preferred, {}) == 2u, "The first preferred map in the player's order wins");
    const auto avoid = M::ParseMapList("fort");
    Require(M::ChooseMapButton(*buttons, {}, avoid) == 2u, "Avoiding a map picks another real map, not Random");
    Require(!M::ChooseMapButton(*buttons, {}, {}), "With no preferences nothing is voted");
    Require(!M::MapVoteButtons(R"({"title":"Shop","buttons":[]})"), "Other forms are left alone");
}

void PacketSenderLearnsTheGamesOwnLayouts() {
    namespace P = Oinkster::PacketSender;
    // A CommandRequest the game sent for "/hub": header, command, then the
    // origin/internal/version bytes kept verbatim.
    const std::vector<std::uint8_t> sent{0x4D, 0x04, '/', 'h', 'u', 'b', 0x00, 0xAA, 0xBB, 0x00, 0x01, 0x00};
    const auto learned = P::LearnCommandTemplate(sent);
    Require(learned && learned->header.size() == 1 && learned->suffix.size() == 6, "The command's surroundings are learned");
    const auto built = P::BuildCommand(*learned, "/q sky");
    const std::vector<std::uint8_t> expected{0x4D, 0x06, '/', 'q', ' ', 's', 'k', 'y', 0x00, 0xAA, 0xBB, 0x00, 0x01, 0x00};
    Require(built == expected, "A new command reuses the learned layout exactly");
    Require(!P::LearnCommandTemplate(std::vector<std::uint8_t>{0x4D, 0x02, 'h', 'i', 0x00}), "Only slash commands are learned");
    Require(!P::LearnCommandTemplate(std::vector<std::uint8_t>{0x09, 0x02, '/', 'a', 0x00}), "Other packets are not commands");

    const std::vector<std::uint8_t> answered{0x65, 0x07, 0x01, 0x02, '1', '\n', 0x00};
    const auto form = P::LearnFormTemplate(answered);
    Require(form && form->beforeResponse == std::vector<std::uint8_t>{0x01} && form->suffix == std::vector<std::uint8_t>{0x00},
        "The form response layout is learned around the response string");
    const auto vote = P::BuildFormResponse(*form, 300, "2");
    const std::vector<std::uint8_t> voteBytes{0x65, 0xAC, 0x02, 0x01, 0x01, '2', 0x00};
    Require(vote == voteBytes, "Form ids are varints");
    Require(!P::LearnFormTemplate(std::vector<std::uint8_t>{0x65, 0x07, 0x00, 0x00}), "A closed form is not a template");

    std::optional<P::CommandTemplate> command;
    std::optional<P::FormTemplate> formOut;
    const auto text = P::EncodeTemplates(learned, form, "1.26.5203.0");
    P::DecodeTemplates(text, "1.26.5203.0", command, formOut);
    Require(command && command->suffix == learned->suffix && formOut && formOut->suffix == form->suffix, "Templates persist");
    std::optional<P::CommandTemplate> other;
    std::optional<P::FormTemplate> otherForm;
    P::DecodeTemplates(text, "1.26.6000.0", other, otherForm);
    Require(!other && !otherForm, "Another build never reuses a template");
    other = *learned;
    otherForm = *form;
    P::DecodeTemplates("build=1.26.5203.0\ncommand=zz:00\nform=01:0:00\n", "1.26.5203.0", other, otherForm);
    Require(!other && !otherForm, "Malformed persisted templates clear stale learned layouts and fail closed");
}

void SkinsSaveAndWear() {
    Require(M::SanitizeSkinFileName("Steve:Main?") == L"Steve_Main_", "Skin filenames replace Windows path metacharacters");
    Require(M::SanitizeSkinFileName("CON.txt") == L"_CON.txt", "Reserved Windows device names stay safe even with an extension");
    Require(M::SanitizeSkinFileName("...  ") == L"player", "An empty sanitized name uses a stable fallback");
    Require(M::SanitizeSkinFileName(std::string(80, 'a')).size() == 48, "Skin filenames are bounded");
    std::vector<std::uint8_t> rgba(2 * 2 * 4, 0x7F);
    const auto png = M::EncodePng(2, 2, rgba);
    Require(png.size() > 57 && png[0] == 0x89 && png[1] == 'P' && png[12] == 'I' && png[13] == 'H', "A PNG starts with its signature and IHDR");
    Require(std::string(png.end() - 8, png.end() - 4) == "IEND", "and ends with IEND");
    Require(M::EncodePng(2, 2, std::vector<std::uint8_t>(3)).empty(), "A wrong-sized image encodes nothing");

    Oinkster::RakNet::Writer add;
    add.VarU32(1);
    WritePlayerListEntry(add, 0x44, "Delta", "2535400000000004", true);
    const auto parsed = M::ParsePlayerList(add.Data());
    Require(parsed && parsed->players.size() == 1, "The entry parses");
    const auto& skin = parsed->players[0].serializedSkin;
    Require(skin.size() > 64 * 64 * 4 && skin[0] == 7 && std::string(skin.begin() + 1, skin.begin() + 8) == "skin-id",
        "The serialized skin is kept from its ID onwards");
    Require(skin.back() == '=', "and ends with its profile hash");
    std::array<std::uint8_t, 16> uuid{};
    uuid.fill(0x5A);
    const auto packet = M::BuildPlayerSkinPacket(uuid, skin, "", "");
    Require(packet[0] == 93 && packet[1] == 0x5A && packet.size() == 1 + 16 + skin.size() + 2, "PlayerSkin is uuid, skin, two names");
    const auto split = M::SplitPacket(packet);
    Require(split && split->id == 93, "The PlayerSkin header round-trips");
}

}

int main() {
    const std::array<std::pair<const char*, void (*)()>, 89> tests{{
        {"CatalogIdsAreUniqueAndWellFormed", CatalogIdsAreUniqueAndWellFormed},
        {"NullMovementKeepsOnlyTheNewestKey", NullMovementKeepsOnlyTheNewestKey},
        {"ChunkBordersProjectAroundThePlayer", ChunkBordersProjectAroundThePlayer},
        {"PostProcessConstantsAreBounded", PostProcessConstantsAreBounded},
        {"BlueberryParityModulesCompose", BlueberryParityModulesCompose},
        {"PlayerListPacketsBuildARoster", PlayerListPacketsBuildARoster},
        {"ChatTitleAndFormPacketsDecode", ChatTitleAndFormPacketsDecode},
        {"HiveMessagesAreRecognised", HiveMessagesAreRecognised},
        {"PacketSenderLearnsTheGamesOwnLayouts", PacketSenderLearnsTheGamesOwnLayouts},
        {"SkinsSaveAndWear", SkinsSaveAndWear},
        {"NativeCatalogMatchesLauncherJson", NativeCatalogMatchesLauncherJson},
        {"AvailabilityFollowsCapabilities", AvailabilityFollowsCapabilities},
        {"ModuleLifecycleIsIsolated", ModuleLifecycleIsIsolated},
        {"ClickFiltersAreDeterministicAndFailSafe", ClickFiltersAreDeterministicAndFailSafe},
        {"CompetitiveServerPolicyFailsClosed", CompetitiveServerPolicyFailsClosed},
        {"ClickGuiViewsAndFavorites", ClickGuiViewsAndFavorites},
        {"SettingParsingAndNormalization", SettingParsingAndNormalization},
        {"ModuleSettingsRoundTripThroughManager", ModuleSettingsRoundTripThroughManager},
        {"SettingsDocumentRoundTripsAndMigrates", SettingsDocumentRoundTripsAndMigrates},
        {"SettingsProfilesSaveSwitchAndRemove", SettingsProfilesSaveSwitchAndRemove},
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
        {"CrosshairSourceAndPerspective", CrosshairSourceAndPerspective},
        {"CrosshairDrawnAndGameCopy", CrosshairDrawnAndGameCopy},
        {"GameCrosshairTextureSource", GameCrosshairTextureSource},
        {"CameraMathMatchesGameConvention", CameraMathMatchesGameConvention},
        {"VisualPresentationModulesCompose", VisualPresentationModulesCompose},
        {"SubtitlesExpireDeduplicateAndOrient", SubtitlesExpireDeduplicateAndOrient},
        {"ChatPresentationTransforms", ChatPresentationTransforms},
        {"SprintAndFramePacingStayWithinPlayerControls", SprintAndFramePacingStayWithinPlayerControls},
        {"ToggleSneakResetsOutsideGameplay", ToggleSneakResetsOutsideGameplay},
        {"JsonParserIsStrictAndBounded", JsonParserIsStrictAndBounded},
        {"SignatureSelectionFailsClosed", SignatureSelectionFailsClosed},
        {"WorldTelemetryRollsBackOutsideMinecraft", WorldTelemetryRollsBackOutsideMinecraft},
        {"ConcurrentPublishSettingsAndRendering", ConcurrentPublishSettingsAndRendering},
        {"PreservedSkinAndInputTests", PreservedSkinAndInputTests},
        {"PreservedShutdownTests", PreservedShutdownTests},
        {"ReplayFileRoundTripsAndRecovers", ReplayFileRoundTripsAndRecovers},
        {"ReplayTimelineBuildsSamplesAndMarks", ReplayTimelineBuildsSamplesAndMarks},
        {"ReplayTimelineCollapsesRecordingPauses", ReplayTimelineCollapsesRecordingPauses},
        {"ReplayTimelineInterpolatesAndClamps", ReplayTimelineInterpolatesAndClamps},
        {"ReplayTimelineSkipsMalformedCamera", ReplayTimelineSkipsMalformedCamera},
        {"ReplaySummaryReadsPartial", ReplaySummaryReadsPartial},
        {"ReplayCaptureParsesHeadersAndClassifies", ReplayCaptureParsesHeadersAndClassifies},
        {"ReplayModRecordsWorldSessions", ReplayModRecordsWorldSessions},
        {"RakNetFramesAcksAndSplits", RakNetFramesAcksAndSplits},
        {"ReplayPlaybackHandshakeAndPacing", ReplayPlaybackHandshakeAndPacing},
        {"ReplayPlaybackUsesActiveRecordingTime", ReplayPlaybackUsesActiveRecordingTime},
        {"ReplayPlaybackRecreatesTheRecordingPlayer", ReplayPlaybackRecreatesTheRecordingPlayer},
        {"ReplayPlaybackSeeksBothWays", ReplayPlaybackSeeksBothWays},
        {"ReplayInjectorTakesOverOnlyALocalWorld", ReplayInjectorTakesOverOnlyALocalWorld},
        {"ReplayRewindPutsPlacedBlocksBack", ReplayRewindPutsPlacedBlocksBack},
        {"ReplayPlayersAreTrackedAndPicked", ReplayPlayersAreTrackedAndPicked},
        {"ReplayExitLeavesTheWorld", ReplayExitLeavesTheWorld},
        {"ReplayWorldCleanupFindsOnlyItsOwnWorld", ReplayWorldCleanupFindsOnlyItsOwnWorld},
        {"ReplayViewHidesTheRecordingPlayer", ReplayViewHidesTheRecordingPlayer},
        {"ReplayRecorderFoundUnderAnotherListId", ReplayRecorderFoundUnderAnotherListId},
        {"ReplayRecorderFoundInLargeServerList", ReplayRecorderFoundInLargeServerList},
        {"ReplayOldHeightChunksAreModernized", ReplayOldHeightChunksAreModernized},
        {"ServerBlocksBecomeABehaviorPack", ServerBlocksBecomeABehaviorPack},
        {"CameraKeyframesMakeASmoothPath", CameraKeyframesMakeASmoothPath},
        {"ReplayCameraFliesAlongThePath", ReplayCameraFliesAlongThePath},
        {"CameraKeyframesEaseAndCut", CameraKeyframesEaseAndCut},
        {"VideosEncodeToMp4", VideosEncodeToMp4},
        {"ServerNameTagsAreCleanedForDisplay", ServerNameTagsAreCleanedForDisplay},
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
