#include "Oinkster/SoundTelemetry.hpp"
#include "Oinkster/Diagnostics.hpp"
#include "Oinkster/HookManager.hpp"
#include "Oinkster/ModuleManager.hpp"
#include "Oinkster/SafeMemory.hpp"
#include "Oinkster/SignatureScan.hpp"
#include "Oinkster/SupportedBuild.hpp"
#include "Oinkster/UnloadGuard.hpp"
#include "Oinkster/Modules/AccessibilityLogic.hpp"
#include "Oinkster/Modules/HudFormat.hpp"
#include "Oinkster/Modules/TelemetryLogic.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <format>
#include <mutex>
#include <string>

namespace Oinkster::SoundTelemetry {
namespace {

// The client world-sound helper on 1.26.5203 (RVA 0x46A9530), called from 80
// sites in the LevelRenderer's LevelListener handlers (e.g. "random.click",
// "random.orb", "random.pop" in the level-event handler at RVA 0x46BEC90):
//   void (LevelRenderer* this, const std::string& name, const Vec3& position,
//         float volume, float pitch, bool, const void*)
// It culls against the camera (this+0x11B8) using volume * 16 blocks, then
// forwards to SoundPlayerInterface vtable slot 2 with the same arguments.
using PlaySoundFn = void (*)(void*, const void*, const float*, float, float, bool, const void*);

PlaySoundFn originalPlaySound{};
void* playSoundTarget{};
std::atomic<ModuleManager*> moduleManager{};
std::atomic_bool attached{};
std::atomic_uint32_t activeCallbacks{};

constexpr std::size_t MaxEventsPerSecond = 64;
std::mutex rateMutex;
SteadyTime windowStart{};
std::size_t windowCount{};
std::atomic_bool validated{};
std::atomic_uint32_t samplesLogged{};

struct CallbackGuard final {
    CallbackGuard() noexcept { activeCallbacks.fetch_add(1, std::memory_order_acq_rel); }
    ~CallbackGuard() { activeCallbacks.fetch_sub(1, std::memory_order_acq_rel); }
    CallbackGuard(const CallbackGuard&) = delete;
    CallbackGuard& operator=(const CallbackGuard&) = delete;
};

bool TakeRateSlot(SteadyTime now) {
    std::scoped_lock lock(rateMutex);
    if (now - windowStart >= std::chrono::seconds(1)) {
        windowStart = now;
        windowCount = 0;
    }
    if (windowCount >= MaxEventsPerSecond) return false;
    ++windowCount;
    return true;
}

void Observe(const void* name, const float* position, float volume) noexcept {
    auto* modules = moduleManager.load(std::memory_order_acquire);
    if (!modules || !name) return;
    try {
        std::string id;
        if (!SafeMemory::ReadMsvcString(name, 0, id, 96) || id.empty()) return;
        // Bedrock sound event names are dotted lower-case ASCII.
        if (!std::ranges::all_of(id, [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_'; })) return;
        const auto caption = Modules::CaptionForSound(id);
        if (caption.empty()) return;

        SoundEvent event;
        event.occurredAt = SteadyClock::now();
        event.soundId = id;
        event.caption = Modules::FromUtf8(caption);
        event.volume = std::isfinite(volume) ? std::clamp(volume, 0.0F, 4.0F) : 1.0F;
        float xyz[3]{};
        if (position && SafeMemory::ReadField(position, 0, xyz)) {
            const Vec3 point{xyz[0], xyz[1], xyz[2]};
            if (Modules::IsPlausibleWorldPosition(point)) event.position = point;
        }
        if (!TakeRateSlot(event.occurredAt)) return;

        if (!validated.exchange(true)) {
            modules->SetCapability(Capability::SoundEvents, {CapabilityState::Available, {}});
            Diagnostics::LogLine("info", "sound_validated", id);
        }
        if (samplesLogged.fetch_add(1) < 12) {
            Diagnostics::LogLine("info", "sound_sample", std::format("{} positional={}", id, event.position.has_value()));
        }
        modules->Publish(event);
    } catch (...) {}
}

void HookedPlaySound(void* self, const void* name, const float* position, float volume, float pitch, bool flag, const void* extra) {
    const CallbackGuard callback;
    if (attached.load(std::memory_order_acquire)) Observe(name, position, volume);
    if (originalPlaySound) originalPlaySound(self, name, position, volume, pitch, flag, extra);
}

}

bool Install(ModuleManager& modules, std::wstring& detail) noexcept {
    const std::wstring build(SupportedBuild::DisplayVersion);
    // Clang prologue (1.26.5203): frame sizes wildcarded; the volume save
    // (movaps xmm6, xmm3), the volume * 16 cull radius and the listener
    // subtraction from position (r8) pin the function. Verified unique.
    constexpr std::array<std::string_view, 1> signatures{
        "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC ? ? ? ? 48 8D AC 24 ? ? ? ? 0F 29 BD ? ? ? ? 0F 29 B5 ? ? ? ? "
        "48 C7 85 ? ? ? ? FE FF FF FF 0F 28 F3 F3 0F 10 05 ? ? ? ? 0F 28 CB F3 0F 59 C8 F3 0F 10 15 ? ? ? ? "
        "F3 0F C2 D3 01 0F 54 CA 0F 55 D0 F3 41 0F 10 00 F3 41 0F 10 58 04 F3 0F 5C 81 ? ? ? ?",
    };
    const auto selection = SignatureScan::FindUniqueInText(signatures);
    Diagnostics::LogLine(selection.outcome == SignatureScan::Outcome::Unique ? "info" : "warning",
        "sound_telemetry_signature", std::string(SignatureScan::OutcomeName(selection.outcome)));
    const auto fail = [&](std::wstring reason) {
        detail = std::move(reason);
        try { modules.SetCapability(Capability::SoundEvents, {CapabilityState::Unavailable, detail}); } catch (...) {}
        return false;
    };
    if (selection.outcome != SignatureScan::Outcome::Unique) {
        return fail(selection.outcome == SignatureScan::Outcome::Ambiguous
            ? L"The world-sound signature matched more than once in Minecraft " + build + L"; refusing to hook an ambiguous target."
            : L"The world-sound signature was not found in Minecraft " + build + L".");
    }
    void* target = reinterpret_cast<void*>(selection.address);
    if (!HookManager::Create(target, reinterpret_cast<void*>(&HookedPlaySound), reinterpret_cast<void**>(&originalPlaySound))) {
        return fail(L"The world-sound hook could not be installed.");
    }
    playSoundTarget = target;
    validated.store(false);
    samplesLogged.store(0);
    try {
        modules.SetCapability(Capability::SoundEvents, {CapabilityState::Pending,
            L"Waiting for the first world sound in Minecraft " + build + L"."});
    } catch (...) {}
    moduleManager.store(&modules, std::memory_order_release);
    attached.store(true, std::memory_order_release);
    detail = L"Sound telemetry attached (LevelRenderer world sounds).";
    return true;
}

void Remove() noexcept {
    attached.store(false, std::memory_order_release);
    if (auto* modules = moduleManager.exchange(nullptr, std::memory_order_acq_rel)) {
        try { modules->SetCapability(Capability::SoundEvents, {CapabilityState::Unavailable, L"Sound telemetry was detached."}); } catch (...) {}
    }
    if (!playSoundTarget) return;
    (void)HookManager::Disable(playSoundTarget);
    for (unsigned attempt = 0; activeCallbacks.load(std::memory_order_acquire) != 0 && attempt < 200; ++attempt) Sleep(5);
    if (activeCallbacks.load(std::memory_order_acquire) != 0) {
        Diagnostics::LogLine("warning", "sound_telemetry_remove", "a thread is still inside the world-sound detour; leaving its trampoline installed");
        UnloadGuard::Block("sound telemetry trampoline still had a live thread at detach");
        return;
    }
    (void)HookManager::Remove(playSoundTarget);
    playSoundTarget = nullptr;
    originalPlaySound = nullptr;
}

}
