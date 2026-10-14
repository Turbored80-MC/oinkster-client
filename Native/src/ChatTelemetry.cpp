#include "Oinkster/ChatTelemetry.hpp"
#include "Oinkster/Diagnostics.hpp"
#include "Oinkster/HookManager.hpp"
#include "Oinkster/ModuleManager.hpp"
#include "Oinkster/SafeMemory.hpp"
#include "Oinkster/SignatureScan.hpp"
#include "Oinkster/SupportedBuild.hpp"
#include "Oinkster/UnloadGuard.hpp"
#include "Oinkster/Modules/HudFormat.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <format>
#include <mutex>
#include <string>

namespace Oinkster::ChatTelemetry {
namespace {

// The client chat-line sink on 1.26.5203 (RVA 0x6584B60):
//   void (Handler* this, const std::string& line, bool)
// Its 18 callers are the per-TextPacketType handlers of one visitor (vtable
// around RVA 0xE992880): e.g. the whisper handler at RVA 0x6586760 formats
// "chat.type.whisper" and passes the result here.
using AddLineFn = void (*)(void*, const void*, bool);

AddLineFn originalAddLine{};
void* addLineTarget{};
std::atomic<ModuleManager*> moduleManager{};
std::atomic_bool attached{};
std::atomic_uint32_t activeCallbacks{};
std::atomic_bool validated{};
std::atomic_uint32_t samplesLogged{};

constexpr std::size_t MaxLineBytes = 1024;
constexpr std::size_t MaxLinesPerSecond = 40;
std::mutex rateMutex;
SteadyTime windowStart{};
std::size_t windowCount{};

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
    if (windowCount >= MaxLinesPerSecond) return false;
    ++windowCount;
    return true;
}

void Observe(const void* line) noexcept {
    auto* modules = moduleManager.load(std::memory_order_acquire);
    if (!modules || !line) return;
    try {
        std::string text;
        if (!SafeMemory::ReadMsvcString(line, 0, text, MaxLineBytes) || text.empty()) return;
        if (!TakeRateSlot(SteadyClock::now())) return;
        ChatEvent event{Modules::FromUtf8(text), {}, std::chrono::system_clock::now()};
        if (event.message.empty()) return;
        if (!validated.exchange(true)) {
            modules->SetCapability(Capability::ChatEvents, {CapabilityState::Available, {}});
            Diagnostics::LogLine("info", "chat_validated", std::format("first line length={}", text.size()));
        }
        // Lengths only: chat content stays out of the log.
        if (samplesLogged.fetch_add(1) < 5) Diagnostics::LogLine("info", "chat_sample", std::format("length={}", text.size()));
        modules->Publish(event);
    } catch (...) {}
}

void HookedAddLine(void* self, const void* line, bool flag) {
    const CallbackGuard callback;
    if (attached.load(std::memory_order_acquire)) Observe(line);
    if (originalAddLine) originalAddLine(self, line, flag);
}

}

bool Install(ModuleManager& modules, std::wstring& detail) noexcept {
    const std::wstring build(SupportedBuild::DisplayVersion);
    // Clang prologue (1.26.5203): frame sizes wildcarded; the bool saved from
    // r8d, `this` spilled, and the std::string size/capacity test on rdx pin
    // the function. Verified unique.
    constexpr std::array<std::string_view, 1> signatures{
        "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC ? ? ? ? 48 8D AC 24 ? ? ? ? 0F 29 B5 ? ? ? ? "
        "48 C7 85 ? ? ? ? FE FF FF FF 44 89 C3 48 89 CE 48 89 8D ? ? ? ? 0F 57 C0 0F 11 85 ? ? ? ? "
        "0F 11 85 ? ? ? ? 48 8B 7A 10 48 83 7A 18 10 72 03 48 8B 12",
    };
    const auto selection = SignatureScan::FindUniqueInText(signatures);
    Diagnostics::LogLine(selection.outcome == SignatureScan::Outcome::Unique ? "info" : "warning",
        "chat_telemetry_signature", std::string(SignatureScan::OutcomeName(selection.outcome)));
    const auto fail = [&](std::wstring reason) {
        detail = std::move(reason);
        try { modules.SetCapability(Capability::ChatEvents, {CapabilityState::Unavailable, detail}); } catch (...) {}
        return false;
    };
    if (selection.outcome != SignatureScan::Outcome::Unique) {
        return fail(selection.outcome == SignatureScan::Outcome::Ambiguous
            ? L"The chat-line signature matched more than once in Minecraft " + build + L"; refusing to hook an ambiguous target."
            : L"The chat-line signature was not found in Minecraft " + build + L".");
    }
    void* target = reinterpret_cast<void*>(selection.address);
    if (!HookManager::Create(target, reinterpret_cast<void*>(&HookedAddLine), reinterpret_cast<void**>(&originalAddLine))) {
        return fail(L"The chat-line hook could not be installed.");
    }
    addLineTarget = target;
    validated.store(false);
    samplesLogged.store(0);
    try {
        modules.SetCapability(Capability::ChatEvents, {CapabilityState::Pending,
            L"Waiting for the first chat line in Minecraft " + build + L"."});
    } catch (...) {}
    moduleManager.store(&modules, std::memory_order_release);
    attached.store(true, std::memory_order_release);
    detail = L"Chat telemetry attached (client chat-line sink).";
    return true;
}

void Remove() noexcept {
    attached.store(false, std::memory_order_release);
    if (auto* modules = moduleManager.exchange(nullptr, std::memory_order_acq_rel)) {
        try { modules->SetCapability(Capability::ChatEvents, {CapabilityState::Unavailable, L"Chat telemetry was detached."}); } catch (...) {}
    }
    if (!addLineTarget) return;
    (void)HookManager::Disable(addLineTarget);
    for (unsigned attempt = 0; activeCallbacks.load(std::memory_order_acquire) != 0 && attempt < 200; ++attempt) Sleep(5);
    if (activeCallbacks.load(std::memory_order_acquire) != 0) {
        Diagnostics::LogLine("warning", "chat_telemetry_remove", "a thread is still inside the chat-line detour; leaving its trampoline installed");
        UnloadGuard::Block("chat telemetry trampoline still had a live thread at detach");
        return;
    }
    (void)HookManager::Remove(addLineTarget);
    addLineTarget = nullptr;
    originalAddLine = nullptr;
}

}
