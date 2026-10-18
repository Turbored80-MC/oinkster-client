#include "Oinkster/WorldTelemetry.hpp"
#include "Oinkster/ModuleManager.hpp"
#include "Oinkster/HookManager.hpp"
#include "Oinkster/CameraTelemetry.hpp"
#include "Oinkster/CombatTelemetry.hpp"
#include "Oinkster/Diagnostics.hpp"
#include "Oinkster/PlayerSkinTelemetry.hpp"
#include "Oinkster/PresentationAdapter.hpp"
#include "Oinkster/PlayerStateTelemetry.hpp"
#include "Oinkster/ResourcePackTelemetry.hpp"
#include "Oinkster/SafeMemory.hpp"
#include "Oinkster/SessionTelemetry.hpp"
#include "Oinkster/SignatureScan.hpp"
#include "Oinkster/SupportedBuild.hpp"
#include "Oinkster/UnloadGuard.hpp"
#include "Oinkster/Modules/TelemetryLogic.hpp"
#include <array>
#include <chrono>
#include <atomic>
#include <cstring>
#include <format>
#include <mutex>
#include <string>
#include <utility>

namespace Oinkster::WorldTelemetry {
namespace {

// InGamePlayScreen::_renderLevelPrep(ScreenContext&, LevelRenderer&, Actor&)
using RenderLevelPrepFn = void (*)(void*, void*, void*, void*);
RenderLevelPrepFn originalRenderLevelPrep{};
ModuleManager* moduleManager{};
std::atomic_bool attached{};
void* renderLevelPrepTarget{};

// Counts threads currently inside HookedRenderLevelPrep. Remove() disables the
// hook and then waits for this to hit zero before destroying the trampoline,
// the same disable -> drain -> remove sequence PlayerSkinTelemetry and
// MouseInputBlocker use for their hooks.
std::atomic_uint32_t activeCallbacks{};
// Steady-clock ticks of the last frame Minecraft prepared a level for
// rendering; 0 = never. Read from the Present thread to tell "in a world"
// apart from the main menu and other out-of-world screens.
std::atomic<SteadyClock::rep> lastLevelFrameTicks{};
std::mutex viewMutex;
std::optional<View> latestView;
// Render-thread only: throttles publishing to ~60 Hz.
SteadyTime lastPublished{};
constexpr auto PublishInterval = std::chrono::milliseconds(16);
// Render-thread only: periodic diagnostic samples of both camera-vector
// candidates, so a live session can confirm the offset against the game's
// own coordinate readout.
SteadyTime lastSampleLogged{};
unsigned samplesLogged{};
constexpr auto SampleLogInterval = std::chrono::seconds(5);

struct CallbackGuard final {
    CallbackGuard() noexcept { activeCallbacks.fetch_add(1, std::memory_order_acq_rel); }
    ~CallbackGuard() { activeCallbacks.fetch_sub(1, std::memory_order_acq_rel); }
    CallbackGuard(const CallbackGuard&) = delete;
    CallbackGuard& operator=(const CallbackGuard&) = delete;
};

// Re-derived for 1.26.5203 from the build's own image (Tools/SurveySignatures.py
// plus disassembly) - this build is clang-compiled, so Flarial's 1.26.0-1.26.3
// MSVC-era signatures and offsets no longer apply:
//  - The hook target is InGamePlayScreen::_renderLevelPrep, identified by its
//    __FUNCSIG__ descriptor ("virtual void __cdecl InGamePlayScreen::
//    _renderLevelPrep(ScreenContext &, LevelRenderer &, Actor &)") and
//    confirmed as slot 93 of InGamePlayScreen's vtable. Its third argument is
//    the LevelRenderer, so no ClientInstance vtable call is needed.
//  - LevelRenderer+0x468 is the LevelRendererPlayer: _renderLevelPrep passes
//    it as `this` to a LevelRendererPlayer method that writes the 0x654 vec3
//    other LevelRendererPlayer vtable methods read. (Flarial 1.26.0: 0x430.)
//  - LevelRendererPlayer+0x72C is the render camera origin:
//    LevelRendererPlayer::preRenderUpdate subtracts it from world positions
//    to get camera-relative coordinates. (Flarial 1.26.0: 0x704.) 0x654 is a
//    second camera vector copied from the camera after the projection is
//    built; both are logged as world_telemetry_sample until a live session
//    confirms which one tracks the in-game coordinates.
// Every pointer in the chain is read through SafeMemory and the result is
// plausibility-checked, so a wrong offset leaves coordinates blank instead
// of faulting the render thread.
constexpr std::size_t LevelRenderer_LevelRendererPlayer = 0x468;
constexpr std::size_t LevelRendererPlayer_CameraPos = 0x72C;
constexpr std::size_t LevelRendererPlayer_CameraPosAlternate = 0x654;

void LogSample(void* levelRendererPlayer, const float (&position)[3], SteadyTime now) {
    if (samplesLogged >= 3 && now - lastSampleLogged < SampleLogInterval) return;
    float alternate[3]{};
    const bool haveAlternate = SafeMemory::ReadField(levelRendererPlayer, LevelRendererPlayer_CameraPosAlternate, alternate);
    lastSampleLogged = now;
    ++samplesLogged;
    Diagnostics::LogLine("info", "world_telemetry_sample", std::format(
        "camera+0x72C=({:.2f}, {:.2f}, {:.2f}) camera+0x654={}",
        position[0], position[1], position[2],
        haveAlternate ? std::format("({:.2f}, {:.2f}, {:.2f})", alternate[0], alternate[1], alternate[2]) : std::string("unreadable")));
}

void HookedRenderLevelPrep(void* screen, void* screenContext, void* levelRenderer, void* actor) {
    const CallbackGuard callback;
    // Always hand the frame to Minecraft first. If the trampoline is missing
    // the level would never be prepared for rendering, which is why Remove()
    // must never clear it while this hook is still enabled.
    if (originalRenderLevelPrep) originalRenderLevelPrep(screen, screenContext, levelRenderer, actor);
    if (!attached.load(std::memory_order_acquire) || !moduleManager || !levelRenderer) return;
    lastLevelFrameTicks.store(SteadyClock::now().time_since_epoch().count(), std::memory_order_release);

    // _renderLevelPrep's Actor& is the local render player. Reuse this
    // already-validated hook as the skin reader's ownership source instead
    // of depending on a second, build-fragile ClientInstance vtable pattern.
    // PlayerSkinTelemetry re-checks the object's RTTI and every pointer read
    // before accepting an atlas, so a changed argument contract fails closed.
    PlayerSkinTelemetry::ObserveLocalPlayer(actor);
    PresentationAdapter::ObserveLocalPlayer(actor);

    void* levelRendererPlayer{};
    if (!SafeMemory::ReadField(levelRenderer, LevelRenderer_LevelRendererPlayer, levelRendererPlayer)
        || !levelRendererPlayer) {
        return;
    }

    float position[3]{};
    if (!SafeMemory::ReadField(levelRendererPlayer, LevelRendererPlayer_CameraPos, position)) return;

    const auto now = SteadyClock::now();
    LogSample(levelRendererPlayer, position, now);
    if (now - lastPublished < PublishInterval) return;
    PlayerTransform transform{};
    transform.capturedAt = now;
    // The camera is at eye level; modules expect the player's feet, which is
    // what the game's own coordinate readout is based on. (Third person moves
    // the camera away from the player, so the position is only exact in first
    // person.)
    transform.position = Modules::FeetFromEye({position[0], position[1], position[2]});
    // A misread offset yields NaNs or astronomically large values; those are
    // dropped here and never reach a module.
    if (!Modules::IsPlausibleWorldPosition(transform.position)) return;
    // Rotation (and, on its own slower cadence, status effects) come from the
    // same Actor's entt components.
    PlayerStateTelemetry::Observe(actor, transform, now);
    // Ping and world identity, on their own ~1 s cadence.
    SessionTelemetry::Observe(actor, now);
    ResourcePackTelemetry::Observe(actor, now);
    CameraTelemetry::Observe(actor, now);
    // Hurt-timer driven combat events, from the unadjusted eye position.
    CombatTelemetry::Observe(actor, {position[0], position[1], position[2]}, transform, now);
    {
        std::scoped_lock lock(viewMutex);
        latestView = View{now, {position[0], position[1], position[2]}, transform.yawDegrees, transform.pitchDegrees};
    }
    lastPublished = now;
    moduleManager->Publish(transform);
}

}

bool Attached() noexcept { return attached.load(std::memory_order_acquire); }

std::optional<View> LatestView() noexcept {
    std::scoped_lock lock(viewMutex);
    return latestView;
}

bool LevelRenderedWithin(SteadyTime now, SteadyClock::duration window) noexcept {
    const auto ticks = lastLevelFrameTicks.load(std::memory_order_acquire);
    if (ticks == 0) return false;
    return now - SteadyTime(SteadyClock::duration(ticks)) <= window;
}

bool Install(ModuleManager& modules, std::wstring& detail) noexcept {
    moduleManager = &modules;
    const std::wstring build(SupportedBuild::DisplayVersion);

    // InGamePlayScreen::_renderLevelPrep, clang prologue (1.26.5203). The
    // stack-frame sizes and the screen field offset are wildcarded; the
    // argument shuffle (r9 -> r12 Actor, r8 -> r15 LevelRenderer, rdx -> r14
    // ScreenContext) is what pins the function. Verified unique in .text
    // with Tools/SurveySignatures.py.
    constexpr std::array<std::string_view, 1> signatures{
        "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC ? ? ? ? 48 8D AC 24 ? ? ? ? "
        "44 0F 29 9D ? ? ? ? 44 0F 29 95 ? ? ? ? 44 0F 29 8D ? ? ? ? 44 0F 29 85 ? ? ? ? "
        "0F 29 BD ? ? ? ? 0F 29 B5 ? ? ? ? 48 C7 85 ? ? ? ? FE FF FF FF "
        "4D 89 CC 4D 89 C7 49 89 D6 48 8D 81 ? ? ? ? 48 89 85 ? ? ? ? 48 89 8D ? ? ? ? 48 83 B9 ? ? ? ? 00",
    };
    constexpr std::array<const char*, 1> labels{"InGamePlayScreen::_renderLevelPrep (clang, 1.26.5203)"};

    const auto selection = SignatureScan::FindUniqueInText(signatures);
    Diagnostics::LogLine(selection.outcome == SignatureScan::Outcome::Unique ? "info" : "warning",
        "world_telemetry_signature", std::string(SignatureScan::OutcomeName(selection.outcome))
            + (selection.outcome == SignatureScan::Outcome::Unique ? std::string(" via ") + labels[selection.patternIndex] : std::string{}));
    if (selection.outcome != SignatureScan::Outcome::Unique) {
        detail = selection.outcome == SignatureScan::Outcome::Ambiguous
            ? L"The world-telemetry signature matched more than once in Minecraft " + build + L"; refusing to hook an ambiguous target."
            : L"The world-telemetry signature was not found in Minecraft " + build + L".";
        modules.SetCapability(Capability::LocalPlayerPosition, {CapabilityState::Unavailable, detail});
        moduleManager = nullptr;
        return false;
    }

    void* target = reinterpret_cast<void*>(selection.address);
    if (!HookManager::Create(target, reinterpret_cast<void*>(&HookedRenderLevelPrep),
            reinterpret_cast<void**>(&originalRenderLevelPrep))) {
        detail = L"The world-telemetry hook could not be installed.";
        modules.SetCapability(Capability::LocalPlayerPosition, {CapabilityState::Unavailable, detail});
        moduleManager = nullptr;
        return false;
    }

    renderLevelPrepTarget = target;
    PlayerStateTelemetry::Attach(modules);
    CombatTelemetry::Attach(modules);
    ResourcePackTelemetry::Attach(modules);
    CameraTelemetry::Attach(modules);
    lastPublished = {};
    lastSampleLogged = {};
    samplesLogged = 0;
    lastLevelFrameTicks.store(0, std::memory_order_release);
    attached.store(true);
    detail = L"World telemetry attached (camera position).";
    modules.SetCapability(Capability::LocalPlayerPosition, {CapabilityState::Available, {}});
    return true;
}

void Remove() noexcept {
    // Tear our own hook down here rather than leaning on the global
    // HookManager::DisableAll()/RemoveAll() that Dx12Overlay::Remove performs
    // later in BedrockAdapter::Detach. Clearing originalRenderLevelPrep while
    // the detour was still live meant HookedRenderLevelPrep stopped calling
    // Minecraft's real function - every in-game screen went
    // blank - and a thread still inside the detour would run a freed
    // trampoline once RemoveAll()/MH_Uninitialize() ran.
    //
    // Same disable -> drain -> remove order (and same bounded wait) as
    // PlayerSkinTelemetry::Remove and MouseInputBlocker::Remove.
    attached.store(false, std::memory_order_release);
    if (moduleManager) {
        moduleManager->SetCapability(Capability::LocalPlayerPosition,
            {CapabilityState::Unavailable, L"World telemetry was detached."});
    }
    if (renderLevelPrepTarget) {
        (void)HookManager::Disable(renderLevelPrepTarget);
        for (unsigned attempt = 0;
             activeCallbacks.load(std::memory_order_acquire) != 0 && attempt < 200; ++attempt) {
            Sleep(5);
        }
        if (activeCallbacks.load(std::memory_order_acquire) != 0) {
            // A wedged render thread must not turn eject into a permanent
            // hang; leave the trampoline installed and intentionally leak it
            // rather than free memory another thread is executing.
            Diagnostics::LogLine("warning", "world_telemetry_remove",
                "render thread still inside the _renderLevelPrep detour; leaving its trampoline installed");
            UnloadGuard::Block("world telemetry trampoline still had a live thread at detach");
            PlayerStateTelemetry::Detach();
    CombatTelemetry::Detach();
    ResourcePackTelemetry::Detach();
    CameraTelemetry::Detach();
            moduleManager = nullptr;
            return;
        }
        (void)HookManager::Remove(renderLevelPrepTarget);
        renderLevelPrepTarget = nullptr;
        originalRenderLevelPrep = nullptr;
    }
    // The hook is drained (or was never installed), so Observe cannot run.
    PlayerStateTelemetry::Detach();
    CombatTelemetry::Detach();
    ResourcePackTelemetry::Detach();
    CameraTelemetry::Detach();
    moduleManager = nullptr;
}

}
