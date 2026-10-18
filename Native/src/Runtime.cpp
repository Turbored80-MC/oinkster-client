#include "Oinkster/Runtime.hpp"
#include "Oinkster/Diagnostics.hpp"
#include "Oinkster/Dx12Overlay.hpp"
#include "Oinkster/Json.hpp"
#include "Oinkster/MouseInputBlocker.hpp"
#include "Oinkster/ServerPolicy.hpp"
#include <Windows.h>
#include <chrono>
#include <format>
#include <stdexcept>
#include <thread>

namespace Oinkster {
namespace {
void Log(std::string_view level, std::string_view event, std::string_view message) noexcept {
    Diagnostics::LogLine(level, event, message);
}

std::wstring Widen(std::string_view value) {
    return std::wstring(value.begin(), value.end());
}

std::wstring JsonString(std::wstring_view value) {
    std::wstring escaped;
    escaped.reserve(value.size() + 2);
    escaped.push_back(L'"');
    constexpr wchar_t Hex[] = L"0123456789abcdef";
    for (const wchar_t character : value) {
        switch (character) {
        case L'"': escaped += L"\\\""; break;
        case L'\\': escaped += L"\\\\"; break;
        case L'\b': escaped += L"\\b"; break;
        case L'\f': escaped += L"\\f"; break;
        case L'\n': escaped += L"\\n"; break;
        case L'\r': escaped += L"\\r"; break;
        case L'\t': escaped += L"\\t"; break;
        default:
            if (character < 0x20) {
                escaped += L"\\u00";
                escaped.push_back(Hex[(character >> 4) & 0xF]);
                escaped.push_back(Hex[character & 0xF]);
            } else {
                escaped.push_back(character);
            }
            break;
        }
    }
    escaped.push_back(L'"');
    return escaped;
}

// Status-pipe JSON (interfaceVersion 2; "capabilities" is additive and
// optional for the launcher). capabilityReasons is additive so older v2
// launchers continue to accept the payload.
std::wstring StatusJson(const ModuleManager& modules, std::wstring_view state, std::optional<bool> hooksInstalled,
    std::wstring_view inputRouting, std::wstring_view supportedBedrock) {
    std::wstring json = L"{\"interfaceVersion\":2,\"state\":\"" + std::wstring(state) + L"\"";
    json += std::format(L",\"moduleCount\":{}", modules.Size());
    if (!supportedBedrock.empty()) json += L",\"supportedBedrock\":\"" + std::wstring(supportedBedrock) + L"\"";
    if (hooksInstalled) json += *hooksInstalled ? L",\"hooksInstalled\":true" : L",\"hooksInstalled\":false";
    if (!inputRouting.empty()) json += L",\"inputRouting\":\"" + std::wstring(inputRouting) + L"\"";
    const auto kind = ServerPolicy::CurrentSessionKind();
    const auto network = kind == SessionKind::None
        ? std::string_view("unclassified") : ServerPolicy::NetworkId(ServerPolicy::CurrentNetwork());
    json += L",\"serverNetwork\":\"" + Widen(network) + L"\"";
    json += L",\"competitivePolicies\":{";
    json += ServerPolicy::IsAllowed(kind, ServerPolicy::CurrentNetwork(), "dc-prevent")
        ? L"\"dc-prevent\":\"allowed\"" : L"\"dc-prevent\":\"blocked\"";
    json += ServerPolicy::IsAllowed(kind, ServerPolicy::CurrentNetwork(), "cps-cap")
        ? L",\"cps-cap\":\"allowed\"}" : L",\"cps-cap\":\"blocked\"}";
    json += L",\"capabilities\":{";
    const auto capabilities = modules.Capabilities();
    for (std::size_t index = 0; index < capabilities.size(); ++index) {
        if (index != 0) json += L",";
        json += L"\"" + Widen(CapabilityId(static_cast<Capability>(index))) + L"\":\""
            + Widen(CapabilityStateId(capabilities[index].state)) + L"\"";
    }
    json += L"},\"capabilityReasons\":{";
    bool firstReason = true;
    for (std::size_t index = 0; index < capabilities.size(); ++index) {
        if (capabilities[index].reason.empty()) continue;
        if (!firstReason) json += L",";
        firstReason = false;
        json += JsonString(Widen(CapabilityId(static_cast<Capability>(index))));
        json += L":";
        json += JsonString(capabilities[index].reason);
    }
    json += L"}}";
    return json;
}

std::wstring_view InputRouting(MouseInputBlocker::InstallState state) noexcept {
    switch (state) {
    case MouseInputBlocker::InstallState::Ready: return L"gameinput-blocked";
    case MouseInputBlocker::InstallState::Initializing: return L"initializing";
    case MouseInputBlocker::InstallState::Unavailable: return L"window-only";
    }
    return L"window-only";
}

// Settings changes from UI drags arrive every frame; coalesce them so the
// file is written once the player stops dragging, not 20 times a second.
constexpr auto SaveDebounce = std::chrono::milliseconds(400);
}

Runtime::Runtime(std::filesystem::path dataDirectory)
    : dataDirectory_(std::move(dataDirectory)), adapter_(std::make_unique<ReviewedBedrockAdapter>()),
      controlPipe_(dataDirectory_ / "settings.json") {}

int Runtime::Run() noexcept {
    try {
        modules_.SetDiagnosticsSink([](std::string_view level, std::string_view event, std::string_view message) {
            Log(level, event, message);
        });
        for (auto& module : CreateModuleCatalog()) modules_.Register(std::move(module));
        SettingsStore settingsStore(dataDirectory_ / "settings.json");
        auto settings = settingsStore.Load();
        Dx12Overlay::ConfigureControlKeys(settings.clickGuiKey, settings.unloadKey);
        // Before attach, game-backed modules are not available yet; their
        // persisted "enabled" preference is deferred, not dropped, and they
        // switch on when their capability validates.
        ApplySettings(modules_, settings);

        const auto supported = adapter_->SupportedVersion();
        const bool statusStarted = statusPipe_.Start(StatusJson(modules_, L"validating", std::nullopt, {}, supported));
        const bool controlStarted = controlPipe_.Start();
        Log("info", "runtime_started", "Native runtime initialized");
        Log(statusStarted ? "info" : "error", "status_pipe", statusStarted ? "Status pipe started" : "Status pipe failed to start");
        Log(controlStarted ? "info" : "error", "control_pipe", controlStarted ? "Control pipe started" : "Control pipe failed to start");

        const auto validation = adapter_->Validate();
        if (validation.status != AdapterStatus::Ready) {
            statusPipe_.Update(StatusJson(modules_, L"review-required", false, {}, supported));
            Log("warning", "adapter_rejected", "No hooks installed; adapter review required");
            statusPipe_.Stop();
            controlPipe_.Stop();
            return 2;
        }
        Log("info", "adapter_validated", "Exact Bedrock package identity validated");
        const auto attachment = adapter_->Attach(modules_);
        if (attachment.status != AdapterStatus::Ready) {
            Log("error", "adapter_attach_failed", "Direct3D 12 hook attachment failed");
            adapter_->Detach();
            statusPipe_.Stop();
            controlPipe_.Stop();
            return 3;
        }
        auto mouseInputState = MouseInputBlocker::State();
        Log("info", "input_routing", "Scoped window routing installed; GameInput mouse blocker discovery started");
        statusPipe_.Update(StatusJson(modules_, L"ready", true, InputRouting(mouseInputState), supported));
        auto capabilityRevision = modules_.CapabilityRevision();

        std::optional<std::chrono::steady_clock::time_point> pendingSaveSince;
        const auto saveNow = [&] {
            settings = CaptureSettings(modules_, Dx12Overlay::MenuKey(), Dx12Overlay::UnloadKey(), settings.revision);
            // A transient save failure (AV lock, sharing violation) must not
            // escape to the outer catch, which tears down every hook.
            try {
                settingsStore.Save(settings);
                ++settings.revision;
            } catch (const std::exception& error) {
                Log("warning", "settings_save_failed", error.what());
            } catch (...) {
                Log("warning", "settings_save_failed", "unknown error");
            }
            pendingSaveSince.reset();
        };

        while (!stopping_.load()) {
            const auto nextCapabilityRevision = modules_.CapabilityRevision();
            if (nextCapabilityRevision != capabilityRevision) {
                capabilityRevision = nextCapabilityRevision;
                statusPipe_.Update(StatusJson(modules_, L"ready", true, InputRouting(mouseInputState), supported));
            }
            if (controlPipe_.ConsumeReloadRequested()) {
                auto incoming = settingsStore.Load();
                if (incoming.clickGuiKey != incoming.unloadKey) {
                    settings = std::move(incoming);
                    Dx12Overlay::ConfigureControlKeys(settings.clickGuiKey, settings.unloadKey);
                    ApplySettings(modules_, settings);
                    pendingSaveSince.reset();
                }
            }
            if (mouseInputState == MouseInputBlocker::InstallState::Initializing) {
                std::wstring detail;
                const auto polled = MouseInputBlocker::PollInstall(detail);
                if (polled != mouseInputState) {
                    mouseInputState = polled;
                    statusPipe_.Update(StatusJson(modules_, L"ready", true, InputRouting(mouseInputState), supported));
                }
            }
            if (Dx12Overlay::ConsumeEjectRequested()) {
                statusPipe_.Update(StatusJson(modules_, L"unloading", true, {}, supported));
                Log("info", "eject_requested", "The configured unload key requested a clean client unload");
                stopping_.store(true);
                break;
            }
            if (Dx12Overlay::ConsumeSettingsDirty() && !pendingSaveSince) pendingSaveSince = std::chrono::steady_clock::now();
            if (pendingSaveSince && std::chrono::steady_clock::now() - *pendingSaveSince >= SaveDebounce) saveNow();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        // Flush a change made in the last debounce window (or one the render
        // thread flagged just before unload) so it is not lost.
        if (Dx12Overlay::ConsumeSettingsDirty() || pendingSaveSince) saveNow();
        modules_.DisableAll();
        adapter_->Detach();
        statusPipe_.Stop();
        controlPipe_.Stop();
        Log("info", "runtime_stopped", "Native runtime stopped cleanly");
        return 0;
    } catch (...) {
        modules_.DisableAll();
        if (adapter_) adapter_->Detach();
        statusPipe_.Stop();
        controlPipe_.Stop();
        Log("error", "runtime_crash_contained", "Unhandled native exception contained");
        return 1;
    }
}

}
