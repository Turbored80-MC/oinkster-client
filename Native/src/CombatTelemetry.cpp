#include "Oinkster/CombatTelemetry.hpp"
#include "Oinkster/Diagnostics.hpp"
#include "Oinkster/Dx12Overlay.hpp"
#include "Oinkster/EnttRegistry.hpp"
#include "Oinkster/ModuleManager.hpp"
#include "Oinkster/SafeMemory.hpp"
#include "Oinkster/SupportedBuild.hpp"
#include "Oinkster/Modules/PvpLogic.hpp"
#include <Windows.h>
#include <atomic>
#include <cmath>
#include <format>
#include <optional>
#include <string>
#include <vector>

namespace Oinkster::CombatTelemetry {
namespace {

// Component layouts on 1.26.5203:
//  - MobHurtTimeComponent { int ticks; } (4 bytes): the inlined try_get at
//    RVA 0x20726C8 indexes its payload page with a 4-byte stride.
//  - AABBShapeComponent { AABB aabb; Vec2 dimensions; } (0x20): the accessor
//    at RVA 0x2286B0 indexes with a 32-byte stride; AABB is two float Vec3s.
constexpr std::uint32_t HurtTimeHash = EnttRegistry::TypeHash("MobHurtTimeComponent");
constexpr std::uint32_t AabbShapeHash = EnttRegistry::TypeHash("AABBShapeComponent");
constexpr std::size_t HurtTimeSize = 4;
constexpr std::size_t AabbShapeSize = 0x20;

// Generous: survival reach is 3 blocks, creative 5; the game decides what
// counts, this only picks which entity a click was aimed at.
constexpr double MaxAimDistance = 6.0;
// Upper bound on entities examined for one click.
constexpr std::size_t MaxCandidates = 256;

std::atomic<ModuleManager*> moduleManager{};
// Render-thread only.
bool validated{};
bool leftWasDown{};
Modules::AttackConfirmer confirmer;
Modules::HurtRiseWatcher localHurt;
std::uint32_t hitsLogged{};

std::optional<int> ReadHurtTicks(const EnttRegistry::EntityRef& entity) noexcept {
    const void* component = EnttRegistry::FindComponent(entity, HurtTimeHash, HurtTimeSize);
    int ticks{};
    if (!component || !SafeMemory::ReadField(component, 0, ticks) || !Modules::IsPlausibleHurtTicks(ticks)) return std::nullopt;
    return ticks;
}

std::optional<Aabb> ReadBounds(const EnttRegistry::EntityRef& entity) noexcept {
    const void* component = EnttRegistry::FindComponent(entity, AabbShapeHash, AabbShapeSize);
    float values[6]{};
    if (!component || !SafeMemory::ReadField(component, 0, values)) return std::nullopt;
    for (const float value : values) {
        if (!std::isfinite(value) || std::abs(value) > 30'000'000.0F) return std::nullopt;
    }
    Aabb box{{values[0], values[1], values[2]}, {values[3], values[4], values[5]}};
    // A mob is at most a few blocks across; anything larger is a misread.
    if (box.maximum.x < box.minimum.x || box.maximum.y < box.minimum.y || box.maximum.z < box.minimum.z
        || box.maximum.x - box.minimum.x > 16.0 || box.maximum.y - box.minimum.y > 16.0 || box.maximum.z - box.minimum.z > 16.0) {
        return std::nullopt;
    }
    return box;
}

bool GameOwnsForeground() noexcept {
    DWORD process{};
    const HWND foreground = GetForegroundWindow();
    return foreground && GetWindowThreadProcessId(foreground, &process) && process == GetCurrentProcessId();
}

// Arms an attack on the entity the click was aimed at, if any.
void ArmAttack(const EnttRegistry::EntityRef& local, const Vec3& eye, const PlayerTransform& transform, SteadyTime now) {
    if (!transform.yawDegrees || !transform.pitchDegrees) return;
    const Vec3 direction = Modules::LookDirection(*transform.yawDegrees, *transform.pitchDegrees);
    std::vector<std::uint32_t> entities;
    if (!EnttRegistry::ReadPoolEntities(local.registry, HurtTimeHash, MaxCandidates, entities)) return;

    std::optional<Modules::AttackConfirmer::Attack> best;
    double bestDistance = MaxAimDistance;
    for (const auto entity : entities) {
        if (entity == local.entity) continue;
        const EnttRegistry::EntityRef candidate{local.registry, entity};
        const auto bounds = ReadBounds(candidate);
        if (!bounds) continue;
        const auto distance = Modules::RayEntryDistance(eye, direction, *bounds);
        if (!distance || *distance > bestDistance) continue;
        const auto hurt = ReadHurtTicks(candidate);
        if (!hurt) continue;
        bestDistance = *distance;
        best = Modules::AttackConfirmer::Attack{now, entity, eye, *bounds, *hurt};
    }
    if (best) confirmer.Arm(*best);
}

}

void Attach(ModuleManager& modules) noexcept {
    validated = false;
    leftWasDown = false;
    confirmer.Clear();
    localHurt.Clear();
    hitsLogged = 0;
    try {
        modules.SetCapability(Capability::CombatEvents, {CapabilityState::Pending,
            L"Waiting for the first validated MobHurtTimeComponent read in Minecraft "
            + std::wstring(SupportedBuild::DisplayVersion) + L"."});
    } catch (...) {}
    moduleManager.store(&modules, std::memory_order_release);
}

void Detach() noexcept {
    auto* modules = moduleManager.exchange(nullptr, std::memory_order_acq_rel);
    if (!modules) return;
    try {
        modules->SetCapability(Capability::CombatEvents, {CapabilityState::Unavailable, L"World telemetry was detached."});
    } catch (...) {}
}

void Observe(const void* actor, const Vec3& eye, const PlayerTransform& transform, SteadyTime now) noexcept {
    auto* modules = moduleManager.load(std::memory_order_acquire);
    if (!modules || !actor) return;
    try {
        EnttRegistry::EntityRef local;
        if (!EnttRegistry::ReadActorEntity(actor, local)) return;
        const auto ownHurt = ReadHurtTicks(local);
        if (!ownHurt) return;
        if (!validated) {
            validated = true;
            modules->SetCapability(Capability::CombatEvents, {CapabilityState::Available, {}});
            Diagnostics::LogLine("info", "combat_validated", std::format("MobHurtTimeComponent ticks={}", *ownHurt));
        }
        if (localHurt.Observe(*ownHurt)) {
            confirmer.Clear();
            modules->Publish(CombatEvent{CombatEventKind::LocalDamaged, now, 0, eye, {}});
            Diagnostics::LogLine("info", "combat_damaged", std::format("ticks={}", *ownHurt));
        }

        // Only a press that Minecraft itself receives can be an attack.
        const bool leftDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0 && GameOwnsForeground() && !Dx12Overlay::IsMenuVisible();
        if (leftDown && !leftWasDown) ArmAttack(local, eye, transform, now);
        leftWasDown = leftDown;

        if (const auto target = confirmer.PendingTarget(now)) {
            const auto hurt = ReadHurtTicks({local.registry, static_cast<std::uint32_t>(*target)});
            if (const auto hit = confirmer.Observe(now, hurt)) {
                modules->Publish(*hit);
                if (hitsLogged < 20) {
                    ++hitsLogged;
                    Diagnostics::LogLine("info", "combat_hit", std::format("distance={:.2f}",
                        Modules::DistanceToAabb(hit->attackerEye, hit->targetBounds)));
                }
            }
        }
    } catch (...) {}
}

}
