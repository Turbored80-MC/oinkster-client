#include "Oinkster/PresentationAdapter.hpp"
#include "Oinkster/CameraTelemetry.hpp"
#include "Oinkster/Diagnostics.hpp"
#include "Oinkster/EnttRegistry.hpp"
#include "Oinkster/HookManager.hpp"
#include "Oinkster/ModuleManager.hpp"
#include "Oinkster/MouseInputBlocker.hpp"
#include "Oinkster/SafeMemory.hpp"
#include "Oinkster/SignatureScan.hpp"
#include "Oinkster/SupportedBuild.hpp"
#include "Oinkster/UnloadGuard.hpp"
#include "Oinkster/Modules/CameraMath.hpp"
#include "Oinkster/Modules/VisualLogic.hpp"
#include <Windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>
#include <numbers>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Oinkster::PresentationAdapter {
namespace {

// Derived from Minecraft 1.26.5203 (RVAs into the dumped image):
//
// CameraUpdate (RVA 0x46CFF30), called through LevelRenderer+0x468 (the
// LevelRendererPlayer) once per rendered frame:
//   void (LevelRendererPlayer* this, void* frame, float alpha)
// It asks CameraLookup for the active camera, takes that entity's
// MinecraftCamera::CameraComponent (entt hash 0x4F6047C7, 0x120 bytes) and
// builds the projection from its vertical FOV in radians at +0x50
// (tan(fov * 0.5), with the aspect at +0x4C and near/far at +0x54/+0x58).
//
// CameraLookup (RVA 0x9ED0A0), called from CameraUpdate and one other site:
//   Out* (Out* out, Source* source)
// Out is { EntityRegistry*, entt::basic_registry*, EntityId, bool valid }
// at +0x00/+0x08/+0x10/+0x18. The pool keyed 0x119E772B picks the entity;
// its twin at RVA 0x9ECE10 differs only in that hash, which is why the
// signature runs up to it.
//
// Options getters (vtable slots 0x3F8 and 0x1F8): Options::getOption(id),
// follow OptionImpl+0x238 overrides, return the value. gfx_gamma is registered
// as FloatOption 0x32 (range 0..1, default 0.5, value at +0x18); view_bobbing
// as BoolOption 0x26 (value at +0x10). Each id is read only by its getter.
//
// Options::getPlayerViewPerspective (RVA 0xFB7430, vtable slot 0x3C0): the
// same getter shape for IntOption 3 (value at +0x18): 0 first person, 1
// third person back, 2 third person front. CameraTelemetry reads the option
// by hand; the camera system asks this getter.
//
// TimeOfDay (RVA 0x2312080), a leaf: float (void*, int time, float alpha)
//   t = ((time % 24000) + alpha) / 24000 - 0.25, wrapped to [0, 1), then
//   eased through the cosine table - the sky's time of day.
using CameraUpdateFn = void* (*)(void*, void*, float, void*);
using CameraLookupFn = void* (*)(void*, void*);
using GammaFn = float (*)(void*);
using ViewBobFn = bool (*)(void*);
using PerspectiveFn = int (*)(void*);
using TimeOfDayFn = float (*)(void*, int, float);
// Hooks whose targets take only integer/pointer arguments; all eight
// argument slots are forwarded so stack arguments survive the detour.
using PassthroughFn = void* (*)(void*, void*, void*, void*, void*, void*, void*, void*);

constexpr std::uint32_t CameraComponentHash = 0x4F6047C7U;   // MinecraftCamera::CameraComponent
constexpr std::size_t CameraComponentSize = 0x120;
constexpr std::size_t CameraRotation = 0x30;                 // camera-to-world quaternion x, y, z, w
constexpr std::size_t CameraPosition = 0x40;                 // world position, copied to LevelRendererPlayer+0x654
constexpr std::size_t CameraFov = 0x50;
// The actor's interpolated render position (a Vec3, 12-byte pool elements).
constexpr std::uint32_t RenderPositionHash = EnttRegistry::TypeHash("RenderPositionComponent");
constexpr std::size_t RenderPositionSize = 0xC;
// Mouse counts to degrees at the vanilla default sensitivity.
constexpr float DegreesPerMouseCount = 0.15F;
// A third-person camera nearer or farther than this from the player is not
// the vanilla boom (spectator cameras, /camera presets); leave it alone.
constexpr float MinimumBoomDistance = 0.3F;
constexpr float MaximumBoomDistance = 64.0F;
constexpr std::size_t MaxRestores = 8;
constexpr std::uint32_t ActorOwnerHash = EnttRegistry::TypeHash("ActorOwnerComponent");
constexpr std::size_t ActorHurtColor = 0x2B0;
constexpr std::array<float, 4> VanillaHurtColor{1.0F, 0.0F, 0.0F, 0.6F};
constexpr std::size_t MaxTintedActors = 4096;
constexpr std::size_t HurtActorsPerFrame = 24;
constexpr std::size_t WeatherRainPrevious = 0x34;
constexpr std::size_t WeatherRain = 0x38;
constexpr std::size_t WeatherLightningPrevious = 0x40;
constexpr std::size_t WeatherLightning = 0x44;
constexpr std::size_t RenderContextScreen = 0x28;
constexpr std::size_t ScreenCamera = 0x18;
constexpr std::size_t CameraWorldStack = 0x40;
constexpr std::size_t DequeMap = 0x08;
constexpr std::size_t DequeMapSize = 0x10;
constexpr std::size_t DequeOffset = 0x18;
constexpr std::size_t DequeSize = 0x20;
// Actor+0x1D8 is the actor's ILevel* (see SessionTelemetry). ILevel vtable
// slot 0xA78 returns the block hit result and 0xA80 the liquid one: the
// HitResultComponent writer at RVA 0x88DBF0 calls both and copies them as
// 0x88-byte HitResults (start +0x00, ray +0x0C, type +0x18 (0 = block),
// face +0x1C, BlockPos +0x20). The getter must be a plain field access,
// which is then read directly; it is checked per Level vtable.
constexpr std::size_t ActorLevel = 0x1D8;
constexpr std::size_t LevelHitResultSlot = 0xA78 / sizeof(void*);
constexpr std::size_t HitStart = 0x00;
constexpr std::size_t HitType = 0x18;
constexpr std::size_t HitFace = 0x1C;
constexpr std::size_t HitBlock = 0x20;
constexpr float MaximumHitDistance = 32.0F;
constexpr std::uint32_t BoomComponentHash = 0xEF14FB9AU;     // MinecraftCamera::CameraThirdPersonBoomComponent
constexpr std::size_t BoomComponentSize = 0x10;
constexpr std::size_t LookupRegistry = 0x08;
constexpr std::size_t LookupEntity = 0x10;
constexpr std::size_t LookupValid = 0x18;
constexpr float MinimumPlausibleFov = 0.1F;                  // radians
constexpr float MaximumPlausibleFov = 3.0F;
constexpr auto SnapshotLifetime = std::chrono::seconds(1);
constexpr auto BoomProbeInterval = std::chrono::seconds(5);
constexpr unsigned MaxBoomProbes = 12;

struct Hook {
    std::string_view name;
    void* target{};
};

std::atomic<ModuleManager*> moduleManager{};
std::atomic_bool attached{};
std::atomic_uint32_t activeCallbacks{};

CameraUpdateFn originalCameraUpdate{};
CameraLookupFn originalCameraLookup{};
GammaFn originalGamma{};
ViewBobFn originalViewBob{};
TimeOfDayFn originalTimeOfDay{};
PassthroughFn originalWeatherTick{};
PassthroughFn originalVignette{};
PassthroughFn originalRenderItem{};
PerspectiveFn originalPerspective{};
std::array<Hook, 9> hooks{{{"camera-update"}, {"camera-lookup"}, {"gamma"}, {"view-bob"}, {"time-of-day"},
    {"weather-tick"}, {"vignette"}, {"render-item"}, {"perspective"}}};

// Written by the camera update (render thread) once per frame; read by the
// option and time hooks, which may run on other threads. A snapshot older
// than SnapshotLifetime (no level rendering) counts as no request.
std::atomic<std::int64_t> snapshotAt{};
std::atomic_bool lightingActive{};
std::atomic<float> gammaValue{};
std::atomic_bool bobbingOff{};
std::atomic_bool timeActive{};
std::atomic_int timeTicks{};
std::atomic<DWORD> renderThread{};
std::atomic_bool weatherActive{};
std::atomic<float> weatherRain{1.0F};
std::atomic<float> weatherThunder{1.0F};
std::atomic_bool vignetteHidden{};
std::atomic_bool handActive{};
std::atomic<float> handX{};
std::atomic<float> handY{};
std::atomic<float> handZ{};
std::atomic<float> handScale{1.0F};
// -1 = no override, else the IntOption value (0..2).
std::atomic_int perspectiveValue{-1};

std::atomic_bool cameraValidated{};
std::atomic_bool transformValidated{};
std::atomic_bool pivotValidated{};
std::atomic_bool damageValidated{};
std::atomic_bool weatherValidated{};
std::atomic_bool overlaysValidated{};
std::atomic_bool handValidated{};
std::atomic_bool selectionValidated{};
std::atomic_bool lightingValidated{};
std::atomic_bool timeValidated{};
std::atomic_bool perspectiveValidated{};

// Render-thread only.
thread_local bool insideCameraUpdate{};
Modules::PresentationProfile frameProfile;
struct Restore {
    void* address{};
    float value{};
};
std::array<Restore, MaxRestores> restores{};
std::size_t restoreCount{};
bool cameraApplied{};
bool fovOverriding{};
float fovCurrentDegrees{};
float fovTransitionSeconds{0.12F};
SteadyTime fovLastStep{};
SteadyTime lastBoomProbe{};
unsigned boomProbes{};
// The camera this frame renders from, after overrides (for the outline).
bool haveView{};
Modules::CameraMath::Float3 viewPosition{};
Modules::CameraMath::Quat viewRotation{};
float viewFov{};
// Hit-result getter resolution, per Level vtable.
const void* hitLevelVtable{};
std::size_t hitOffset{};
bool hitThroughPointer{};
bool hitResolved{};
bool hitFailureLogged{};
// Camera transform state.
SteadyTime lastTransformStep{};
bool cinematicActive{};
Modules::CameraMath::Quat cinematicRotation{};
float freelookYaw{};
float freelookPitch{};
bool freelookCapturing{};
float eyeOffset{};
// The local player, from WorldTelemetry's _renderLevelPrep hook.
std::atomic<const void*> localRegistry{};
std::atomic<std::uint32_t> localEntity{};
std::atomic<const void*> localActor{};
// Hurt color pass state.
std::vector<std::uint32_t> hurtEntities;
std::size_t hurtCursor{};
bool hurtTintApplied{};
bool hurtRestoring{};
bool hurtPassStarted{};

struct CallbackGuard final {
    CallbackGuard() noexcept { activeCallbacks.fetch_add(1, std::memory_order_acq_rel); }
    ~CallbackGuard() { activeCallbacks.fetch_sub(1, std::memory_order_acq_rel); }
    CallbackGuard(const CallbackGuard&) = delete;
    CallbackGuard& operator=(const CallbackGuard&) = delete;
};

std::int64_t NowTicks() noexcept { return SteadyClock::now().time_since_epoch().count(); }

bool SnapshotFresh() noexcept {
    const auto at = snapshotAt.load(std::memory_order_acquire);
    return at != 0 && SteadyClock::duration(NowTicks() - at) < SnapshotLifetime;
}

void MarkAvailable(Capability capability, std::atomic_bool& flag, std::string_view event, const std::string& message) noexcept {
    if (flag.exchange(true)) return;
    auto* modules = moduleManager.load(std::memory_order_acquire);
    if (!modules) return;
    try {
        modules->SetCapability(capability, {CapabilityState::Available, {}});
        Diagnostics::LogLine("info", std::string(event), message);
    } catch (...) {}
}

// The only writes this adapter makes to game memory: one float, into a
// committed, writable page, SEH-guarded.
bool WriteFloat(void* address, float value) noexcept {
    MEMORY_BASIC_INFORMATION information{};
    if (!address || VirtualQuery(address, &information, sizeof(information)) != sizeof(information)) return false;
    constexpr DWORD writable = PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if (information.State != MEM_COMMIT || (information.Protect & writable) == 0 || (information.Protect & PAGE_GUARD) != 0) return false;
    return SafeMemory::CopyGuarded(address, &value, sizeof(value));
}

// Writes a float the camera update reads and remembers the game's value so
// HookedCameraUpdate can put it back once the frame's camera is built.
bool OverrideFloat(void* address, float value) noexcept {
    float original{};
    if (restoreCount >= restores.size() || !SafeMemory::ReadField(address, 0, original)) return false;
    if (!WriteFloat(address, value)) return false;
    restores[restoreCount++] = {address, original};
    return true;
}

void RestoreOverrides() noexcept {
    // Newest first, so a field written twice ends on the game's value.
    while (restoreCount > 0) {
        const auto& entry = restores[--restoreCount];
        (void)WriteFloat(entry.address, entry.value);
    }
}

void PublishSnapshot(const Modules::PresentationProfile& profile) noexcept {
    lightingActive.store(profile.lighting.has_value(), std::memory_order_relaxed);
    if (profile.lighting) gammaValue.store(std::clamp(profile.lighting->multiplier, 1.0F, 10.0F), std::memory_order_relaxed);
    bobbingOff.store(profile.viewBobbing && profile.viewBobbing->strength <= 0.001F, std::memory_order_relaxed);
    timeActive.store(profile.time.has_value(), std::memory_order_relaxed);
    if (profile.time) timeTicks.store(std::clamp(profile.time->ticks, 0, 23999), std::memory_order_relaxed);
    weatherActive.store(profile.weather.has_value(), std::memory_order_relaxed);
    if (profile.weather) {
        // Rain and snow are one precipitation level in Weather; the snow
        // channel cannot be separated from it yet, so rain drives both.
        weatherRain.store(std::clamp(profile.weather->rain, 0.0F, 1.0F), std::memory_order_relaxed);
        weatherThunder.store(std::clamp(profile.weather->thunder, 0.0F, 1.0F), std::memory_order_relaxed);
    }
    vignetteHidden.store(profile.screenEffects && profile.screenEffects->vignette <= 0.001F, std::memory_order_relaxed);
    handActive.store(profile.handView.has_value(), std::memory_order_relaxed);
    if (profile.handView) {
        handX.store(std::clamp(profile.handView->offsetX, -0.5F, 0.5F), std::memory_order_relaxed);
        handY.store(std::clamp(profile.handView->offsetY, -0.5F, 0.5F), std::memory_order_relaxed);
        handZ.store(std::clamp(profile.handView->offsetZ, -0.5F, 0.5F), std::memory_order_relaxed);
        handScale.store(std::clamp(profile.handView->scale, 0.5F, 1.5F), std::memory_order_relaxed);
    }
    int perspective = -1;
    if (profile.perspective) {
        switch (profile.perspective->perspective) {
        case CameraPerspective::FirstPerson: perspective = 0; break;
        case CameraPerspective::ThirdPersonBack: perspective = 1; break;
        case CameraPerspective::ThirdPersonFront: perspective = 2; break;
        case CameraPerspective::Unknown: break;
        }
    }
    perspectiveValue.store(perspective, std::memory_order_relaxed);
    snapshotAt.store(NowTicks(), std::memory_order_release);
}

// Eases the override toward the requested FOV (or back to vanilla once
// nothing requests one) and returns the degrees to render with, or nothing
// when the vanilla value should be left alone.
std::optional<float> StepFov(float vanillaDegrees, SteadyTime now) noexcept {
    const auto& profile = frameProfile;
    const bool requested = profile.zoom.has_value() || profile.fov.has_value();
    if (profile.zoom) fovTransitionSeconds = std::clamp(profile.zoom->transitionSeconds, 0.0F, 1.0F);
    if (!requested && !fovOverriding) return std::nullopt;
    const float target = requested ? Modules::EffectiveFov(profile, vanillaDegrees) : vanillaDegrees;
    if (!fovOverriding) {
        fovOverriding = true;
        fovCurrentDegrees = vanillaDegrees;
        fovLastStep = now;
    }
    const float elapsed = std::clamp(std::chrono::duration<float>(now - fovLastStep).count(), 0.0F, 0.1F);
    fovLastStep = now;
    // About 95% of the way there after the module's transition time.
    const float blend = fovTransitionSeconds <= 0.0F ? 1.0F : 1.0F - std::exp(-3.0F * elapsed / fovTransitionSeconds);
    fovCurrentDegrees += (target - fovCurrentDegrees) * blend;
    if (!requested && std::abs(fovCurrentDegrees - vanillaDegrees) < 0.05F) {
        fovOverriding = false;
        return std::nullopt;
    }
    return fovCurrentDegrees;
}

void ProbeBoom(const EnttRegistry::EntityRef& entity, SteadyTime now) noexcept {
    if (boomProbes >= MaxBoomProbes || now - lastBoomProbe < BoomProbeInterval) return;
    lastBoomProbe = now;
    const void* boom = EnttRegistry::FindComponent(entity, BoomComponentHash, BoomComponentSize);
    std::array<float, 4> values{};
    if (!boom || !SafeMemory::ReadField(boom, 0, values)) return;
    ++boomProbes;
    try {
        Diagnostics::LogLine("info", "presentation_boom_probe",
            std::format("{:.3f} {:.3f} {:.3f} {:.3f}", values[0], values[1], values[2], values[3]));
    } catch (...) {}
}

std::optional<Modules::CameraMath::Float3> LocalRenderPosition() noexcept {
    EnttRegistry::EntityRef player{localRegistry.load(std::memory_order_acquire), localEntity.load(std::memory_order_acquire)};
    if (!player.registry) return std::nullopt;
    const void* component = EnttRegistry::FindComponent(player, RenderPositionHash, RenderPositionSize);
    std::array<float, 3> xyz{};
    if (!component || !SafeMemory::ReadField(component, 0, xyz)) return std::nullopt;
    if (!std::isfinite(xyz[0]) || !std::isfinite(xyz[1]) || !std::isfinite(xyz[2])) return std::nullopt;
    return Modules::CameraMath::Float3{xyz[0], xyz[1], xyz[2]};
}

void MarkTransformAvailable() noexcept {
    if (transformValidated.exchange(true)) return;
    auto* modules = moduleManager.load(std::memory_order_acquire);
    if (!modules) return;
    try {
        modules->SetCapability(Capability::CameraRotation, {CapabilityState::Available, {}});
        modules->SetCapability(Capability::CameraMotion, {CapabilityState::Available, {}});
        Diagnostics::LogLine("info", "presentation_transform_validated",
            std::format("camera rotation and position readable; mouse capture {}",
                MouseInputBlocker::State() == MouseInputBlocker::InstallState::Ready ? "ready" : "not ready"));
    } catch (...) {}
}

void StepFreelook(const Modules::PresentationProfile& profile, float elapsed) noexcept {
    const bool active = profile.cameraRotation && profile.cameraRotation->active;
    if (active != freelookCapturing) {
        MouseInputBlocker::SetLookCapture(active);
        freelookCapturing = active;
    }
    if (active) {
        std::int64_t deltaX{};
        std::int64_t deltaY{};
        MouseInputBlocker::TakeLookDelta(deltaX, deltaY);
        const float scale = DegreesPerMouseCount * std::clamp(profile.cameraRotation->sensitivity, 0.1F, 3.0F)
            * Modules::CameraMath::Pi / 180.0F;
        freelookYaw -= static_cast<float>(deltaX) * scale;
        freelookPitch -= static_cast<float>(deltaY) * scale;
        return;
    }
    // Released (or the module was turned off): ease back to the player's view.
    const float seconds = profile.cameraRotation ? profile.cameraRotation->returnSeconds : 0.0F;
    const float keep = seconds <= 0.0F ? 0.0F : std::exp(-3.0F * elapsed / seconds);
    freelookYaw *= keep;
    freelookPitch *= keep;
    if (std::abs(freelookYaw) < 1e-4F && std::abs(freelookPitch) < 1e-4F) freelookYaw = freelookPitch = 0.0F;
}

// Hurt-camera roll, cinematic smoothing, freelook and third-person distance,
// applied to the active camera for this frame's camera update only.
void ApplyTransform(void* camera, SteadyTime now) noexcept {
    namespace CM = Modules::CameraMath;
    std::array<float, 4> rotation{};
    std::array<float, 3> position{};
    if (!SafeMemory::ReadField(camera, CameraRotation, rotation) || !SafeMemory::ReadField(camera, CameraPosition, position)) return;
    const CM::Quat original{rotation[0], rotation[1], rotation[2], rotation[3]};
    const CM::Float3 originalPosition{position[0], position[1], position[2]};
    const float norm = std::sqrt(CM::Dot(original, original));
    if (!CM::IsFinite(original) || std::abs(norm - 1.0F) > 0.01F
        || !std::isfinite(position[0]) || !std::isfinite(position[1]) || !std::isfinite(position[2])) {
        return;
    }
    MarkTransformAvailable();

    const auto& profile = frameProfile;
    const float elapsed = std::clamp(std::chrono::duration<float>(now - lastTransformStep).count(), 0.0F, 0.1F);
    lastTransformStep = now;
    const auto perspective = CameraTelemetry::Current();
    const bool thirdPerson = perspective == CameraPerspective::ThirdPersonBack || perspective == CameraPerspective::ThirdPersonFront;

    const auto renderPosition = LocalRenderPosition();
    if (renderPosition && perspective == CameraPerspective::FirstPerson) {
        // In first person the camera sits at the eye: learn how far above
        // the render position that is (it changes while sneaking).
        const CM::Float3 offset = originalPosition - *renderPosition;
        if (std::abs(offset.x) < 0.3F && std::abs(offset.z) < 0.3F && std::abs(offset.y) < 2.5F) {
            eyeOffset += (offset.y - eyeOffset) * std::min(1.0F, elapsed * 10.0F);
        }
    }
    if (renderPosition && !pivotValidated.exchange(true)) {
        if (auto* modules = moduleManager.load(std::memory_order_acquire)) {
            try {
                modules->SetCapability(Capability::ThirdPersonPresentation, {CapabilityState::Available, {}});
                Diagnostics::LogLine("info", "presentation_pivot_validated", std::format(
                    "render position {:.2f} {:.2f} {:.2f}, camera {:.2f} {:.2f} {:.2f}", renderPosition->x, renderPosition->y,
                    renderPosition->z, originalPosition.x, originalPosition.y, originalPosition.z));
            } catch (...) {}
        }
    }

    CM::Quat rotationOut = original;
    if (profile.hurtCamera) rotationOut = CM::ScaleRoll(rotationOut, std::clamp(profile.hurtCamera->strength, 0.0F, 1.0F));

    if (profile.cinematicCamera && profile.cinematicCamera->strength > 0.0F) {
        if (!cinematicActive) {
            cinematicRotation = rotationOut;
            cinematicActive = true;
        }
        // Strength 0..1 maps to a 30/s .. 1.5/s follow rate.
        const float rate = 30.0F + (1.5F - 30.0F) * std::clamp(profile.cinematicCamera->strength, 0.0F, 1.0F);
        cinematicRotation = CM::Slerp(cinematicRotation, rotationOut, 1.0F - std::exp(-rate * elapsed));
        rotationOut = cinematicRotation;
    } else {
        cinematicActive = false;
    }

    StepFreelook(profile, elapsed);
    if (freelookYaw != 0.0F || freelookPitch != 0.0F) {
        const float basePitch = CM::Pitch(rotationOut);
        freelookPitch = std::clamp(freelookPitch, -CM::MaxPitchRadians - basePitch, CM::MaxPitchRadians - basePitch);
        rotationOut = CM::OffsetYawPitch(rotationOut, freelookYaw, freelookPitch);
    }

    CM::Float3 positionOut = originalPosition;
    if (thirdPerson && renderPosition) {
        const CM::Float3 eye = *renderPosition + CM::Float3{0.0F, eyeOffset, 0.0F};
        const CM::Float3 forward = CM::Normalize(CM::Forward(original));
        const float vanillaDistance = CM::DistanceAlongView(originalPosition, forward, eye);
        if (vanillaDistance > MinimumBoomDistance && vanillaDistance < MaximumBoomDistance) {
            // Orbit the point on the vanilla view ray nearest the eye, so the
            // framing matches the game's own boom.
            const CM::Float3 pivot = originalPosition + forward * vanillaDistance;
            const float distance = Modules::EffectiveThirdPersonDistance(profile, perspective, vanillaDistance);
            const bool rotated = std::abs(CM::Dot(rotationOut, original)) < 0.999999F;
            if (rotated || std::abs(distance - vanillaDistance) > 1e-3F) {
                positionOut = pivot - CM::Normalize(CM::Forward(rotationOut)) * distance;
            }
        }
    }

    auto* bytes = static_cast<std::uint8_t*>(camera);
    if (std::abs(CM::Dot(rotationOut, original)) < 0.999999F) {
        const std::array<float, 4> values{rotationOut.x, rotationOut.y, rotationOut.z, rotationOut.w};
        for (std::size_t index = 0; index < values.size(); ++index) {
            (void)OverrideFloat(bytes + CameraRotation + index * sizeof(float), values[index]);
        }
    }
    if (CM::Length(positionOut - originalPosition) > 1e-4F) {
        const std::array<float, 3> values{positionOut.x, positionOut.y, positionOut.z};
        for (std::size_t index = 0; index < values.size(); ++index) {
            (void)OverrideFloat(bytes + CameraPosition + index * sizeof(float), values[index]);
        }
    }
    viewPosition = positionOut;
    viewRotation = rotationOut;
    haveView = true;
}

const std::uint8_t* HitResultAddress(const void* level) noexcept {
    const void* vtable{};
    if (!SafeMemory::ReadField(level, 0, vtable) || !vtable) return nullptr;
    if (vtable != hitLevelVtable) {
        hitLevelVtable = vtable;
        hitResolved = false;
        const auto* code = static_cast<const std::uint8_t*>(SafeMemory::ReadVirtualSlot(level, LevelHitResultSlot));
        std::array<std::uint8_t, 8> bytes{};
        if (code && SafeMemory::ReadField(code, 0, bytes) && bytes[0] == 0x48 && (bytes[1] == 0x8D || bytes[1] == 0x8B)) {
            if (bytes[2] == 0x81 && bytes[7] == 0xC3) {          // lea/mov rax, [rcx+disp32]; ret
                std::int32_t displacement{};
                std::memcpy(&displacement, bytes.data() + 3, sizeof(displacement));
                if (displacement > 0 && displacement < 0x10000) {
                    hitOffset = static_cast<std::size_t>(displacement);
                    hitResolved = true;
                }
            } else if (bytes[2] == 0x41 && bytes[4] == 0xC3) {   // lea/mov rax, [rcx+disp8]; ret
                hitOffset = bytes[3];
                hitResolved = true;
            }
            hitThroughPointer = bytes[1] == 0x8B;
        }
        if (!hitResolved && !hitFailureLogged) {
            hitFailureLogged = true;
            std::string text;
            for (const auto byte : bytes) text += std::format("{:02X} ", byte);
            Diagnostics::LogLine("warning", "presentation_hit_result_unvalidated", "ILevel::getHitResult is not a field getter: " + text);
        }
    }
    if (!hitResolved) return nullptr;
    if (!hitThroughPointer) return static_cast<const std::uint8_t*>(level) + hitOffset;
    const void* result{};
    return SafeMemory::ReadField(level, hitOffset, result) ? static_cast<const std::uint8_t*>(result) : nullptr;
}

void PublishSelection(SteadyTime now) noexcept {
    namespace CM = Modules::CameraMath;
    auto* modules = moduleManager.load(std::memory_order_acquire);
    const void* actor = localActor.load(std::memory_order_acquire);
    if (!modules || !actor || !haveView) return;
    const void* level{};
    if (!SafeMemory::ReadField(actor, ActorLevel, level) || !level) return;
    const auto* hit = HitResultAddress(level);
    std::array<float, 3> start{};
    std::int32_t type{};
    std::int8_t face{};
    std::array<std::int32_t, 3> block{};
    if (!hit || !SafeMemory::ReadField(hit, HitStart, start) || !SafeMemory::ReadField(hit, HitType, type)
        || !SafeMemory::ReadField(hit, HitFace, face) || !SafeMemory::ReadField(hit, HitBlock, block)) {
        return;
    }
    const CM::Float3 origin{start[0], start[1], start[2]};
    if (type < 0 || type > 3 || face < -1 || face > 5 || !std::isfinite(origin.x) || !std::isfinite(origin.y) || !std::isfinite(origin.z)
        || CM::Length(origin - viewPosition) > MaximumHitDistance) {
        return;
    }
    MarkAvailable(Capability::SelectedBlockRender, selectionValidated, "presentation_selection_validated",
        std::format("hit type {} block {} {} {} face {}", type, block[0], block[1], block[2], face));
    BlockSelectionEvent event;
    event.capturedAt = now;
    const CM::Float3 center{static_cast<float>(block[0]) + 0.5F, static_cast<float>(block[1]) + 0.5F, static_cast<float>(block[2]) + 0.5F};
    event.hasBlock = type == 0 && CM::Length(center - origin) < MaximumHitDistance;
    event.block = block;
    event.face = static_cast<BlockFace>(face);
    event.cameraPosition = {viewPosition.x, viewPosition.y, viewPosition.z};
    event.cameraRotation = {viewRotation.x, viewRotation.y, viewRotation.z, viewRotation.w};
    event.verticalFov = viewFov;
    try { modules->Publish(event); } catch (...) {}
}

void PublishPose(SteadyTime now) noexcept {
    auto* modules = moduleManager.load(std::memory_order_acquire);
    if (!modules) return;
    CameraPoseEvent event;
    event.capturedAt = now;
    event.position = {viewPosition.x, viewPosition.y, viewPosition.z};
    event.rotation = {viewRotation.x, viewRotation.y, viewRotation.z, viewRotation.w};
    event.verticalFov = viewFov;
    try { modules->Publish(event); } catch (...) {}
}

void ApplyCamera(void* out) noexcept {
    std::uint8_t valid{};
    EnttRegistry::EntityRef entity{};
    if (cameraApplied || !out || !SafeMemory::ReadField(out, LookupValid, valid) || valid != 1
        || !SafeMemory::ReadField(out, LookupRegistry, entity.registry) || !entity.registry
        || !SafeMemory::ReadField(out, LookupEntity, entity.entity)) {
        return;
    }
    cameraApplied = true;
    const auto now = SteadyClock::now();
    ProbeBoom(entity, now);
    auto* camera = const_cast<void*>(EnttRegistry::FindComponent(entity, CameraComponentHash, CameraComponentSize));
    float radians{};
    if (!camera || !SafeMemory::ReadField(camera, CameraFov, radians) || !std::isfinite(radians)
        || radians < MinimumPlausibleFov || radians > MaximumPlausibleFov) {
        return;
    }
    const float vanillaDegrees = radians * 180.0F / std::numbers::pi_v<float>;
    MarkAvailable(Capability::CameraPresentation, cameraValidated, "presentation_camera_validated",
        std::format("vanilla vertical fov {:.1f} deg", vanillaDegrees));
    viewFov = radians;
    if (const auto degrees = StepFov(vanillaDegrees, now)) {
        const float overridden = *degrees * std::numbers::pi_v<float> / 180.0F;
        if (OverrideFloat(static_cast<std::uint8_t*>(camera) + CameraFov, overridden)) viewFov = overridden;
    }
    haveView = false;
    ApplyTransform(camera, now);
    const bool wantsSelection = frameProfile.blockOutline || (frameProfile.cameraPose && frameProfile.cameraPose->selection);
    if (wantsSelection) PublishSelection(now);
    if (frameProfile.cameraPose && haveView) PublishPose(now);
}

void* HookedCameraLookup(void* out, void* source) {
    const CallbackGuard callback;
    void* result = originalCameraLookup ? originalCameraLookup(out, source) : out;
    if (insideCameraUpdate && attached.load(std::memory_order_acquire)) ApplyCamera(out);
    return result;
}

void StepHurtColor() noexcept;

void* HookedCameraUpdate(void* self, void* frame, float alpha, void* extra) {
    const CallbackGuard callback;
    if (!attached.load(std::memory_order_acquire)) {
        return originalCameraUpdate ? originalCameraUpdate(self, frame, alpha, extra) : nullptr;
    }
    renderThread.store(GetCurrentThreadId(), std::memory_order_relaxed);
    if (auto* modules = moduleManager.load(std::memory_order_acquire)) {
        try {
            frameProfile = Modules::CollectPresentationProfile(*modules);
        } catch (...) {
            frameProfile = {};
        }
        PublishSnapshot(frameProfile);
    }
    StepHurtColor();
    insideCameraUpdate = true;
    cameraApplied = false;
    void* result = originalCameraUpdate ? originalCameraUpdate(self, frame, alpha, extra) : nullptr;
    insideCameraUpdate = false;
    // The view and projection are built (and the position copied into the
    // renderer); hand the game back its own camera so nothing else - camera
    // blending, the next frame's systems - sees the override.
    RestoreOverrides();
    return result;
}

float HookedGamma(void* options) {
    const CallbackGuard callback;
    const float vanilla = originalGamma ? originalGamma(options) : 0.5F;
    if (!attached.load(std::memory_order_acquire)) return vanilla;
    if (std::isfinite(vanilla) && vanilla >= 0.0F && vanilla <= 1.5F) {
        MarkAvailable(Capability::LightingPresentation, lightingValidated, "presentation_lighting_validated",
            std::format("vanilla gamma {:.2f}", vanilla));
    }
    if (SnapshotFresh() && lightingActive.load(std::memory_order_relaxed)) return gammaValue.load(std::memory_order_relaxed);
    return vanilla;
}

int HookedPerspective(void* options) {
    const CallbackGuard callback;
    const int vanilla = originalPerspective ? originalPerspective(options) : 0;
    if (!attached.load(std::memory_order_acquire)) return vanilla;
    if (vanilla >= 0 && vanilla <= 2) {
        MarkAvailable(Capability::PerspectivePresentation, perspectiveValidated, "presentation_perspective_validated",
            std::format("vanilla perspective {}", vanilla));
    }
    const int requested = perspectiveValue.load(std::memory_order_relaxed);
    if (requested >= 0 && SnapshotFresh()) return requested;
    return vanilla;
}

bool HookedViewBob(void* options) {
    const CallbackGuard callback;
    const bool vanilla = originalViewBob ? originalViewBob(options) : true;
    if (!attached.load(std::memory_order_acquire)) return vanilla;
    if (SnapshotFresh() && bobbingOff.load(std::memory_order_relaxed)) return false;
    return vanilla;
}

float HookedTimeOfDay(void* self, int time, float alpha) {
    const CallbackGuard callback;
    if (!originalTimeOfDay) return 0.0F;
    // Only the client's render thread: the integrated server of a local
    // world calls the same function for its own (gameplay) time.
    if (!attached.load(std::memory_order_acquire) || GetCurrentThreadId() != renderThread.load(std::memory_order_relaxed)) {
        return originalTimeOfDay(self, time, alpha);
    }
    const float vanilla = originalTimeOfDay(self, time, alpha);
    if (std::isfinite(vanilla) && vanilla >= -0.5F && vanilla <= 1.5F) {
        MarkAvailable(Capability::TimePresentation, timeValidated, "presentation_time_validated",
            std::format("vanilla time of day {:.3f}", vanilla));
    }
    if (SnapshotFresh() && timeActive.load(std::memory_order_relaxed)) {
        return originalTimeOfDay(self, timeTicks.load(std::memory_order_relaxed), 0.0F);
    }
    return vanilla;
}

// --- Hurt color -----------------------------------------------------------
// Every Actor carries its damage tint at +0x2B0 (mce::Color r, g, b, a): the
// Actor constructors (e.g. RVA 0x19EA430, which also reads the Actor's
// EntityContext at +0x10/+0x18) store {1, 0, 0, 0.6} there. Actors are found
// through the client registry's ActorOwnerComponent pool (8-byte elements:
// the owning Actor pointer) and checked against their own EntityContext
// before anything is written. A few per frame, so a crowded world never
// costs a frame.
void StepHurtColor() noexcept {
    const auto& tint = frameProfile.hurtTint;
    if (!tint && !hurtTintApplied) return;
    const void* registry = localRegistry.load(std::memory_order_acquire);
    if (!registry) return;
    const std::array<float, 4> wanted = tint
        ? std::array<float, 4>{static_cast<float>((tint->color >> 16U) & 0xFFU) / 255.0F,
              static_cast<float>((tint->color >> 8U) & 0xFFU) / 255.0F, static_cast<float>(tint->color & 0xFFU) / 255.0F,
              std::clamp(tint->opacity, 0.0F, 1.0F)}
        : VanillaHurtColor;
    // Switching between tinting and restoring starts a fresh full pass, so
    // every actor touched so far is visited again.
    const bool restoring = !tint.has_value();
    if (restoring != hurtRestoring) {
        hurtRestoring = restoring;
        hurtCursor = hurtEntities.size();
        hurtPassStarted = false;
    }
    if (hurtCursor >= hurtEntities.size()) {
        if (restoring && hurtPassStarted) {
            hurtTintApplied = false;
            hurtPassStarted = false;
            hurtEntities.clear();
            hurtCursor = 0;
            return;
        }
        try {
            if (!EnttRegistry::ReadPoolEntities(registry, ActorOwnerHash, MaxTintedActors, hurtEntities)) return;
        } catch (...) {
            return;
        }
        hurtCursor = 0;
        hurtPassStarted = true;
    }
    if (!restoring) hurtTintApplied = true;
    const std::size_t end = std::min(hurtEntities.size(), hurtCursor + HurtActorsPerFrame);
    for (; hurtCursor < end; ++hurtCursor) {
        const EnttRegistry::EntityRef entity{registry, hurtEntities[hurtCursor]};
        const void* owner = EnttRegistry::FindComponent(entity, ActorOwnerHash, sizeof(void*));
        void* actor{};
        const void* actorRegistry{};
        std::uint32_t actorEntity{};
        if (!owner || !SafeMemory::ReadField(owner, 0, actor) || !actor
            || !SafeMemory::ReadField(actor, EnttRegistry::Layout::ActorRegistry, actorRegistry) || actorRegistry != registry
            || !SafeMemory::ReadField(actor, EnttRegistry::Layout::ActorEntity, actorEntity) || actorEntity != entity.entity) {
            continue;
        }
        std::array<float, 4> current{};
        auto* field = static_cast<std::uint8_t*>(actor) + ActorHurtColor;
        if (!SafeMemory::ReadField(field, 0, current)) continue;
        if (!std::ranges::all_of(current, [](float value) { return std::isfinite(value) && value >= 0.0F && value <= 1.0F; })) continue;
        MarkAvailable(Capability::DamageRender, damageValidated, "presentation_hurt_color_validated",
            std::format("actor tint {:.2f} {:.2f} {:.2f} {:.2f}", current[0], current[1], current[2], current[3]));
        if (current == wanted) continue;
        for (std::size_t index = 0; index < wanted.size(); ++index) (void)WriteFloat(field + index * sizeof(float), wanted[index]);
    }
}

// --- Weather --------------------------------------------------------------
// Weather::tick (RVA 0x32AAD60): Weather keeps rain at +0x34 (previous) /
// +0x38 (current) / +0x3C (target) and lightning at +0x40 / +0x44 / +0x48;
// Weather::levelEvent (RVA 0x32AAD00) sets the targets from START/STOP_RAIN
// and START/STOP_THUNDER. The game's own levels are kept in a shadow copy
// and put back before every tick, so weather keeps its real progression and
// only the rendered levels are scaled.
struct WeatherShadow {
    void* weather{};
    std::array<float, 2> rain{};
    std::array<float, 2> lightning{};
};
WeatherShadow weatherShadow;
bool weatherScaled{};

void RestoreWeather() noexcept {
    if (!weatherScaled || !weatherShadow.weather) return;
    auto* bytes = static_cast<std::uint8_t*>(weatherShadow.weather);
    (void)WriteFloat(bytes + WeatherRainPrevious, weatherShadow.rain[0]);
    (void)WriteFloat(bytes + WeatherRain, weatherShadow.rain[1]);
    (void)WriteFloat(bytes + WeatherLightningPrevious, weatherShadow.lightning[0]);
    (void)WriteFloat(bytes + WeatherLightning, weatherShadow.lightning[1]);
    weatherScaled = false;
}

void* HookedWeatherTick(void* weather, void* a2, void* a3, void* a4, void* a5, void* a6, void* a7, void* a8) {
    const CallbackGuard callback;
    if (!originalWeatherTick) return nullptr;
    if (!attached.load(std::memory_order_acquire) || GetCurrentThreadId() != renderThread.load(std::memory_order_relaxed)) {
        return originalWeatherTick(weather, a2, a3, a4, a5, a6, a7, a8);
    }
    if (weatherShadow.weather != weather) {
        RestoreWeather();
        weatherShadow.weather = weather;
    }
    RestoreWeather();
    void* result = originalWeatherTick(weather, a2, a3, a4, a5, a6, a7, a8);
    std::array<float, 4> levels{};
    auto* bytes = static_cast<std::uint8_t*>(weather);
    if (!SafeMemory::ReadField(bytes, WeatherRainPrevious, levels)) return result;      // +0x34..+0x40
    float lightning{};
    if (!SafeMemory::ReadField(bytes, WeatherLightning, lightning)) return result;
    if (!std::ranges::all_of(levels, [](float value) { return std::isfinite(value) && value >= -0.01F && value <= 1.01F; })) return result;
    MarkAvailable(Capability::WeatherPresentation, weatherValidated, "presentation_weather_validated",
        std::format("rain {:.2f} lightning {:.2f}", levels[1], lightning));
    if (!SnapshotFresh() || !weatherActive.load(std::memory_order_relaxed)) return result;
    weatherShadow.rain = {levels[0], levels[1]};
    weatherShadow.lightning = {levels[3], lightning};
    const float rain = weatherRain.load(std::memory_order_relaxed);
    const float thunder = weatherThunder.load(std::memory_order_relaxed);
    (void)WriteFloat(bytes + WeatherRainPrevious, levels[0] * rain);
    (void)WriteFloat(bytes + WeatherRain, levels[1] * rain);
    (void)WriteFloat(bytes + WeatherLightningPrevious, levels[3] * thunder);
    (void)WriteFloat(bytes + WeatherLightning, lightning * thunder);
    weatherScaled = true;
    return result;
}

// --- Vignette -------------------------------------------------------------
// The HUD's "vignette_renderer" UI custom renderer (factory RVA 0x45A28B0,
// vtable slot 16 = render, RVA 0x8DD5C00). Skipped while its opacity is 0.
void* HookedVignette(void* self, void* a2, void* a3, void* a4, void* a5, void* a6, void* a7, void* a8) {
    const CallbackGuard callback;
    if (!originalVignette) return nullptr;
    if (attached.load(std::memory_order_acquire)) {
        MarkAvailable(Capability::ScreenEffectsPresentation, overlaysValidated, "presentation_overlays_validated", "vignette renderer");
        if (SnapshotFresh() && vignetteHidden.load(std::memory_order_relaxed)) return nullptr;
    }
    return originalVignette(self, a2, a3, a4, a5, a6, a7, a8);
}

// --- Hand view ------------------------------------------------------------
// ItemInHandRenderer::renderItem (RVA 0x56095C0; 8 arguments, the actor in
// [r8]). It pushes a copy of the mce::Camera world matrix stack's top
// ([[rdx+0x28]+0x18] is the camera; the stack is an MSVC std::deque<Matrix>
// at +0x40 with map +0x48, map size +0x50, offset +0x58, size +0x60, one
// 64-byte matrix per block) and builds the item transform on it. For the
// local player in first person the top is offset and scaled first, then
// restored once the item is drawn.
float* WorldMatrixTop(void* context) noexcept {
    const void* screen{};
    void* camera{};
    if (!SafeMemory::ReadField(context, RenderContextScreen, screen) || !screen
        || !SafeMemory::ReadField(screen, ScreenCamera, camera) || !camera) {
        return nullptr;
    }
    std::uintptr_t map{};
    std::size_t mapSize{};
    std::size_t offset{};
    std::size_t size{};
    if (!SafeMemory::ReadField(camera, CameraWorldStack + DequeMap, map) || !map
        || !SafeMemory::ReadField(camera, CameraWorldStack + DequeMapSize, mapSize)
        || !SafeMemory::ReadField(camera, CameraWorldStack + DequeOffset, offset)
        || !SafeMemory::ReadField(camera, CameraWorldStack + DequeSize, size)) {
        return nullptr;
    }
    if (size == 0 || size > 64 || mapSize == 0 || mapSize > 4096 || (mapSize & (mapSize - 1)) != 0) return nullptr;
    float* top{};
    if (!SafeMemory::ReadField(reinterpret_cast<const void*>(map), ((offset + size - 1) & (mapSize - 1)) * sizeof(void*), top) || !top) {
        return nullptr;
    }
    return SafeMemory::IsReadable(top, sizeof(float) * 16) ? top : nullptr;
}

void* HookedRenderItem(void* self, void* context, void* actorRef, void* a4, void* a5, void* a6, void* a7, void* a8) {
    const CallbackGuard callback;
    if (!originalRenderItem) return nullptr;
    const auto call = [&] { return originalRenderItem(self, context, actorRef, a4, a5, a6, a7, a8); };
    const void* actor{};
    if (!attached.load(std::memory_order_acquire) || !actorRef || !SafeMemory::ReadField(actorRef, 0, actor)
        || !actor || actor != localActor.load(std::memory_order_acquire) || !context) {
        return call();
    }
    float* top = WorldMatrixTop(context);
    if (!top) return call();
    MarkAvailable(Capability::HandPresentation, handValidated, "presentation_hand_validated", "first-person item matrix readable");
    if (!SnapshotFresh() || !handActive.load(std::memory_order_relaxed) || CameraTelemetry::Current() != CameraPerspective::FirstPerson) {
        return call();
    }
    std::array<float, 16> original{};
    if (!SafeMemory::ReadField(top, 0, original)) return call();
    const float scale = handScale.load(std::memory_order_relaxed);
    const std::array<float, 3> offset{handX.load(std::memory_order_relaxed), handY.load(std::memory_order_relaxed),
        handZ.load(std::memory_order_relaxed)};
    // top * Translate(offset) * Scale(scale), column-major.
    std::array<float, 16> adjusted = original;
    for (std::size_t row = 0; row < 4; ++row) {
        adjusted[12 + row] = original[row] * offset[0] + original[4 + row] * offset[1] + original[8 + row] * offset[2] + original[12 + row];
        for (std::size_t column = 0; column < 3; ++column) adjusted[column * 4 + row] = original[column * 4 + row] * scale;
    }
    for (std::size_t index = 0; index < adjusted.size(); ++index) (void)WriteFloat(top + index, adjusted[index]);
    void* result = call();
    // Always hand the game back its own matrix, even after a partial write.
    for (std::size_t index = 0; index < original.size(); ++index) (void)WriteFloat(top + index, original[index]);
    return result;
}

// 0x15D bytes up to the active-camera pool hash; RIP-relative displacements
// wildcarded. Verified unique (Tools/SurveySignatures.py).
constexpr std::string_view CameraLookupSignature =
    "55 56 57 53 48 83 EC 58 48 8D 6C 24 50 48 C7 45 00 FE FF FF FF 49 89 D0 48 89 CE 48 8B 02 48 8B 80 38 05 00 00 48 8D 55 "
    "E8 4C 89 C1 FF 15 ? ? 70 0E 48 83 7D E8 00 75 3B 48 8D 05 ? ? 32 0E 48 89 44 24 20 48 8D 0D ? ? 21 0E 48 8D 15 ? "
    "? 25 0E 4C 8D 0D ? ? 25 0E 41 B8 2C 01 00 00 E8 ? ? 89 FF 84 C0 74 0B C7 04 25 00 00 00 00 DE C0 AD DE 48 8B 45 E8 "
    "80 38 00 75 3B 48 8D 05 ? ? 32 0E 48 89 44 24 20 48 8D 0D ? ? 29 0E 48 8D 15 ? ? 29 0E 4C 8D 0D ? ? 25 0E 41 B8 "
    "30 01 00 00 E8 ? ? 89 FF 84 C0 74 0B C7 04 25 00 00 00 00 DE C0 AD DE 48 8B 4D F8 48 8B 01 48 8B 40 10 48 8D 55 D8 FF "
    "15 ? ? 70 0E 0F 57 C0 48 8B 7D F0 0F 11 45 E8 48 85 FF 74 67 F0 FF 4F 08 75 25 48 8B 07 48 8B 00 48 89 F9 FF 15 ? ? "
    "70 0E F0 FF 4F 0C 75 10 48 8B 07 48 8B 40 08 48 89 F9 FF 15 ? ? 70 0E 48 8B 7D F0 48 C7 45 F8 00 00 00 00 48 85 FF 74 "
    "2B F0 FF 4F 08 75 25 48 8B 07 48 8B 00 48 89 F9 FF 15 ? ? 70 0E F0 FF 4F 0C 75 10 48 8B 07 48 8B 40 08 48 89 F9 FF 15 "
    "? ? 70 0E 48 8B 5D D8 48 85 DB 0F 84 93 00 00 00 48 8D 7B 30 48 89 F9 BA 2B 77 9E 11";

struct Target {
    std::string_view name;
    std::string_view signature;
    void* detour;
    void** original;
    std::size_t hook;
};

void SetStatus(ModuleManager& modules, Capability capability, CapabilityState state, std::wstring detail) noexcept {
    try { modules.SetCapability(capability, {state, std::move(detail)}); } catch (...) {}
}


}

bool Install(ModuleManager& modules, std::wstring& detail) noexcept {
    const std::wstring build(SupportedBuild::DisplayVersion);
    const std::array<Target, 9> targets{{
        {"camera-update",
            "41 57 41 56 41 54 56 57 53 48 81 EC ? ? ? ? 44 0F 29 AC 24 ? ? ? ? 44 0F 29 A4 24 ? ? ? ? "
            "44 0F 29 9C 24 ? ? ? ? 44 0F 29 94 24 ? ? ? ? 44 0F 29 8C 24 ? ? ? ? 44 0F 29 84 24 ? ? ? ? "
            "0F 29 BC 24 ? ? ? ? 0F 29 B4 24 ? ? ? ? 49 89 D6 48 89 CE C6 42 38 01 48 8B 42 20 48 8B 4A 18 "
            "48 01 C8 48 FF C8",
            reinterpret_cast<void*>(&HookedCameraUpdate), reinterpret_cast<void**>(&originalCameraUpdate), 0},
        {"camera-lookup", CameraLookupSignature,
            reinterpret_cast<void*>(&HookedCameraLookup), reinterpret_cast<void**>(&originalCameraLookup), 1},
        {"gamma",
            "48 83 EC 38 48 8B 05 ? ? ? ? 48 31 E0 48 89 44 24 30 48 8B 01 48 8B 40 08 48 8D 54 24 28 "
            "41 B8 32 00 00 00 FF 15 ? ? ? ? 48 8B 4C 24 28 48 89 C8 48 8B 49 08 48 8B 89 38 02 00 00 "
            "48 85 C9 75 ED F3 0F 10 40 18",
            reinterpret_cast<void*>(&HookedGamma), reinterpret_cast<void**>(&originalGamma), 2},
        {"view-bob",
            "48 83 EC 38 48 8B 05 ? ? ? ? 48 31 E0 48 89 44 24 30 48 8B 01 48 8B 40 08 48 8D 54 24 28 "
            "41 B8 26 00 00 00 FF 15 ? ? ? ? 48 8B 4C 24 28 48 89 C8 48 8B 49 08 48 8B 89 38 02 00 00 "
            "48 85 C9 75 ED 0F B6 40 10",
            reinterpret_cast<void*>(&HookedViewBob), reinterpret_cast<void**>(&originalViewBob), 3},
        {"time-of-day",
            "48 63 C2 48 69 C8 F1 19 76 05 48 89 CA 48 C1 EA 3F 48 C1 F9 29 01 D1 69 C9 C0 5D 00 00 "
            "29 C8 F3 0F 2A C0 F3 0F 58 C2 F3 0F 5E 05",
            reinterpret_cast<void*>(&HookedTimeOfDay), reinterpret_cast<void**>(&originalTimeOfDay), 4},
        {"weather-tick",
            "56 48 83 EC 20 48 8B 41 58 80 B8 00 01 00 00 01 0F 85 ? ? ? ? 48 89 CE FF 41 30 F3 0F 10 41 38 "
            "F3 0F 10 51 44 F3 0F 11 41 34 F3 0F 11 51 40 F3 0F 10 49 48",
            reinterpret_cast<void*>(&HookedWeatherTick), reinterpret_cast<void**>(&originalWeatherTick), 5},
        {"vignette",
            "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC ? ? ? ? 48 8D AC 24 ? ? ? ? 44 0F 29 9D ? ? ? ? "
            "44 0F 29 95 ? ? ? ? 44 0F 29 8D ? ? ? ? 44 0F 29 85 ? ? ? ? 0F 29 BD ? ? ? ? 0F 29 B5 ? ? ? ? "
            "48 C7 85 ? ? ? ? FE FF FF FF 4D 89 CE 4C 89 C3 48 89 D7 48 89 CE 49 8B 00 48 8B 80 D0 06 00 00 "
            "48 8D 55 C8 4C 89 C1 FF 15",
            reinterpret_cast<void*>(&HookedVignette), reinterpret_cast<void**>(&originalVignette), 6},
        {"render-item",
            "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC ? ? ? ? 48 8D AC 24 ? ? ? ? 44 0F 29 AD ? ? ? ? "
            "44 0F 29 A5 ? ? ? ? 44 0F 29 9D ? ? ? ? 44 0F 29 95 ? ? ? ? 44 0F 29 8D ? ? ? ? 44 0F 29 85 ? ? ? ? "
            "0F 29 BD ? ? ? ? 0F 29 B5 ? ? ? ? 48 C7 85 ? ? ? ? FE FF FF FF 49 8B 38 48 85 FF 0F 84 ? ? ? ? "
            "4D 89 CF 4C 8B 5F 10 8B 47 18 45 8B 4B 50 4D 8B 43 48 45 29 C1 41 C1 E9 03 41 FF C9 41 81 E1 F2 E8 6A 0A",
            reinterpret_cast<void*>(&HookedRenderItem), reinterpret_cast<void**>(&originalRenderItem), 7},
        {"perspective",
            "48 83 EC 38 48 8B 05 ? ? ? ? 48 31 E0 48 89 44 24 30 48 8B 01 48 8B 40 08 48 8D 54 24 28 "
            "41 B8 03 00 00 00 FF 15 ? ? ? ? 48 8B 4C 24 28 48 89 C8 48 8B 49 08 48 8B 89 38 02 00 00 "
            "48 85 C9 75 ED 8B 40 18",
            reinterpret_cast<void*>(&HookedPerspective), reinterpret_cast<void**>(&originalPerspective), 8},
    }};

    cameraValidated.store(false);
    transformValidated.store(false);
    pivotValidated.store(false);
    damageValidated.store(false);
    weatherValidated.store(false);
    overlaysValidated.store(false);
    handValidated.store(false);
    selectionValidated.store(false);
    hitLevelVtable = nullptr;
    hitResolved = false;
    hitFailureLogged = false;
    lightingValidated.store(false);
    timeValidated.store(false);
    perspectiveValidated.store(false);
    perspectiveValue.store(-1);
    snapshotAt.store(0);
    boomProbes = 0;
    moduleManager.store(&modules, std::memory_order_release);

    bool cameraHooks = true;
    std::wstring failures;
    for (const auto& target : targets) {
        const std::array<std::string_view, 1> signatures{target.signature};
        const auto selection = SignatureScan::FindUniqueInText(signatures);
        const bool unique = selection.outcome == SignatureScan::Outcome::Unique;
        Diagnostics::LogLine(unique ? "info" : "warning", "presentation_signature",
            std::string(target.name) + " " + std::string(SignatureScan::OutcomeName(selection.outcome)));
        bool hooked = false;
        if (unique) {
            void* address = reinterpret_cast<void*>(selection.address);
            hooked = HookManager::Create(address, target.detour, target.original);
            if (hooked) hooks[target.hook].target = address;
        }
        if (!hooked) {
            if (target.hook <= 1) cameraHooks = false;
            failures += (failures.empty() ? L"" : L", ") + std::wstring(target.name.begin(), target.name.end());
        }
    }

    const auto pending = [&](Capability capability, bool installed, std::wstring_view what) {
        SetStatus(modules, capability, installed ? CapabilityState::Pending : CapabilityState::Unavailable, installed
            ? L"Waiting for the " + std::wstring(what) + L" hook to validate in Minecraft " + build + L"."
            : L"The " + std::wstring(what) + L" hook could not be installed for Minecraft " + build + L".");
    };
    pending(Capability::CameraPresentation, cameraHooks, L"camera FOV");
    pending(Capability::CameraRotation, cameraHooks, L"camera rotation");
    pending(Capability::CameraMotion, cameraHooks, L"camera rotation");
    pending(Capability::ThirdPersonPresentation, cameraHooks, L"camera position");
    pending(Capability::LightingPresentation, hooks[2].target != nullptr, L"gamma");
    pending(Capability::DamageRender, cameraHooks, L"actor hurt color");
    pending(Capability::WeatherPresentation, hooks[5].target != nullptr, L"weather");
    pending(Capability::ScreenEffectsPresentation, hooks[6].target != nullptr, L"vignette");
    pending(Capability::HandPresentation, hooks[7].target != nullptr, L"item-in-hand");
    pending(Capability::SelectedBlockRender, cameraHooks, L"block hit result");
    pending(Capability::TimePresentation, hooks[4].target != nullptr, L"time-of-day");
    pending(Capability::PerspectivePresentation, hooks[8].target != nullptr, L"camera perspective");

    attached.store(true, std::memory_order_release);
    detail = failures.empty() ? L"Presentation adapter attached (camera, view bobbing, gamma, time of day, weather, vignette, hand, hurt color)."
                              : L"Presentation adapter attached without: " + failures + L".";
    return cameraHooks || hooks[2].target || hooks[4].target;
}

void Remove() noexcept {
    attached.store(false, std::memory_order_release);
    MouseInputBlocker::SetLookCapture(false);
    if (auto* modules = moduleManager.exchange(nullptr, std::memory_order_acq_rel)) {
        for (const auto capability : {Capability::CameraPresentation, Capability::CameraRotation, Capability::CameraMotion,
                 Capability::ThirdPersonPresentation, Capability::LightingPresentation, Capability::TimePresentation,
                 Capability::DamageRender, Capability::WeatherPresentation, Capability::ScreenEffectsPresentation,
                 Capability::HandPresentation, Capability::SelectedBlockRender, Capability::PerspectivePresentation}) {
            SetStatus(*modules, capability, CapabilityState::Unavailable, L"The presentation adapter was detached.");
        }
    }
    for (auto& hook : hooks) {
        if (hook.target) (void)HookManager::Disable(hook.target);
    }
    for (unsigned attempt = 0; activeCallbacks.load(std::memory_order_acquire) != 0 && attempt < 200; ++attempt) Sleep(5);
    if (activeCallbacks.load(std::memory_order_acquire) != 0) {
        Diagnostics::LogLine("warning", "presentation_remove", "a thread is still inside a presentation detour; leaving trampolines installed");
        UnloadGuard::Block("presentation adapter trampoline still had a live thread at detach");
        return;
    }
    for (auto& hook : hooks) {
        if (hook.target) (void)HookManager::Remove(hook.target);
        hook.target = nullptr;
    }
    originalCameraUpdate = nullptr;
    originalCameraLookup = nullptr;
    originalGamma = nullptr;
    originalViewBob = nullptr;
    originalTimeOfDay = nullptr;
    originalWeatherTick = nullptr;
    originalVignette = nullptr;
    originalRenderItem = nullptr;
    originalPerspective = nullptr;
    perspectiveValue.store(-1);
    // Hand the game back the levels and tints it would have shown.
    RestoreWeather();
    weatherShadow = {};
    frameProfile.hurtTint.reset();
    for (unsigned step = 0; hurtTintApplied && step < 100000; ++step) StepHurtColor();
    fovOverriding = false;
    freelookCapturing = false;
    freelookYaw = freelookPitch = 0.0F;
    cinematicActive = false;
}

std::optional<CameraPerspective> PerspectiveOverride() noexcept {
    if (!attached.load(std::memory_order_acquire) || !perspectiveValidated.load(std::memory_order_acquire) || !SnapshotFresh()) {
        return std::nullopt;
    }
    switch (perspectiveValue.load(std::memory_order_relaxed)) {
    case 0: return CameraPerspective::FirstPerson;
    case 1: return CameraPerspective::ThirdPersonBack;
    case 2: return CameraPerspective::ThirdPersonFront;
    default: return std::nullopt;
    }
}

void ObserveLocalPlayer(const void* actor) noexcept {
    EnttRegistry::EntityRef entity{};
    if (!EnttRegistry::ReadActorEntity(actor, entity)) return;
    localRegistry.store(entity.registry, std::memory_order_release);
    localEntity.store(entity.entity, std::memory_order_release);
    localActor.store(actor, std::memory_order_release);
}

}
