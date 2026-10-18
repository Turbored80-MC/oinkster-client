#include "Oinkster/ScreenTelemetry.hpp"

#include "Oinkster/Diagnostics.hpp"
#include "Oinkster/HookManager.hpp"
#include "Oinkster/SafeMemory.hpp"
#include "Oinkster/SignatureScan.hpp"
#include "Oinkster/UnloadGuard.hpp"
#include <Windows.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>

namespace Oinkster::ScreenTelemetry {
namespace {

// A screen-change listener on 1.26.5203 (RVA 0x87F1F0), the call operator of
// a std::function the game registers for screen events (no direct callers):
//   (Functor* this, ?, const ScreenEvent& event)
//   if (event.+0x98 == 1 && event.name /*std::string at +0x30*/ contains "start_screen")
//       tail-call <react to the title screen>
// The event's name and flag are all that is read here. The return value is
// passed through untouched in case the call is not void.
using ListenerFn = std::uintptr_t (*)(void* self, void* context, const void* event);

constexpr std::size_t EventNameOffset = 0x30;
constexpr std::size_t EventFlagOffset = 0x98;
constexpr std::size_t MaxNameLength = 160;
constexpr std::uint32_t LoggedEvents = 120;

ListenerFn originalListener{};
void* listenerTarget{};
std::atomic_bool attached{};
std::atomic_uint32_t activeCallbacks{};
std::atomic_bool known{};
std::atomic_bool onTitle{};
std::atomic_bool onDisconnected{};
std::atomic_bool localWorldLoad{};
std::atomic_uint32_t eventsLogged{};

struct CallbackGuard final {
    CallbackGuard() noexcept { activeCallbacks.fetch_add(1, std::memory_order_acq_rel); }
    ~CallbackGuard() { activeCallbacks.fetch_sub(1, std::memory_order_acq_rel); }
    CallbackGuard(const CallbackGuard&) = delete;
    CallbackGuard& operator=(const CallbackGuard&) = delete;
};

void Observe(const void* event) noexcept {
    if (!event) return;
    try {
        std::string name;
        std::uint8_t flag{};
        if (!SafeMemory::ReadMsvcString(event, EventNameOffset, name, MaxNameLength)) return;
        if (!SafeMemory::ReadField(event, EventFlagOffset, flag)) return;
        const bool title = name.find("start_screen") != std::string::npos;
        const bool disconnected = name.find("/disconnected") != std::string::npos;
        if (name.find("local_world") != std::string::npos) {
            localWorldLoad.store(true, std::memory_order_release);
        } else if (name.find("joining_multiplayer") != std::string::npos || name.find("external_server") != std::string::npos) {
            localWorldLoad.store(false, std::memory_order_release);
        }
        // flag 1 is the state the game's own title-screen check acts on; any
        // other screen arriving the same way means the title is not on top.
        if (flag == 1) {
            onTitle.store(title, std::memory_order_release);
            onDisconnected.store(disconnected, std::memory_order_release);
            known.store(true, std::memory_order_release);
        } else if (title) {
            onTitle.store(false, std::memory_order_release);
        } else if (disconnected) {
            onDisconnected.store(false, std::memory_order_release);
        }
        if (eventsLogged.fetch_add(1) < LoggedEvents) {
            Diagnostics::LogLine("info", "screen_event", std::format("name={} flag={} onTitle={}",
                name, flag, onTitle.load(std::memory_order_acquire)));
        }
    } catch (...) {}
}

std::uintptr_t HookedListener(void* self, void* context, const void* event) {
    const CallbackGuard callback;
    if (attached.load(std::memory_order_acquire)) Observe(event);
    const auto* original = originalListener;
    return original ? original(self, context, event) : 0;
}

}

bool Install(std::wstring& detail) noexcept {
    constexpr std::array<std::string_view, 1> signatures{
        "56 57 53 48 83 EC 20 41 80 B8 ? ? ? ? 01 75 ? 49 8B 70 ? 48 83 FE 0C 72 ? 49 83 78 ? 10 72 ? "
        "4D 8B 40 ? EB ? 49 83 C0 ? 48 8B 79 08 4C 01 C6 48 8D 05",
    };
    const auto selection = SignatureScan::FindUniqueInText(signatures);
    Diagnostics::LogLine(selection.outcome == SignatureScan::Outcome::Unique ? "info" : "warning",
        "screen_telemetry_signature", std::string(SignatureScan::OutcomeName(selection.outcome)));
    if (selection.outcome != SignatureScan::Outcome::Unique) {
        detail = L"The screen-event signature did not match uniquely; title-screen-only controls are disabled.";
        return false;
    }
    void* target = reinterpret_cast<void*>(selection.address);
    if (!HookManager::Create(target, reinterpret_cast<void*>(&HookedListener), reinterpret_cast<void**>(&originalListener))) {
        detail = L"The screen-event hook could not be installed.";
        return false;
    }
    listenerTarget = target;
    known.store(false);
    onTitle.store(false);
    onDisconnected.store(false);
    localWorldLoad.store(false);
    eventsLogged.store(0);
    attached.store(true, std::memory_order_release);
    detail = L"Screen telemetry attached (title-screen listener).";
    return true;
}

void Remove() noexcept {
    attached.store(false, std::memory_order_release);
    known.store(false, std::memory_order_release);
    onTitle.store(false, std::memory_order_release);
    onDisconnected.store(false, std::memory_order_release);
    if (!listenerTarget) return;
    (void)HookManager::Disable(listenerTarget);
    for (unsigned attempt = 0; activeCallbacks.load(std::memory_order_acquire) != 0 && attempt < 200; ++attempt) Sleep(5);
    if (activeCallbacks.load(std::memory_order_acquire) != 0) {
        Diagnostics::LogLine("warning", "screen_telemetry_remove", "a thread is still inside the screen-event detour; leaving its trampoline installed");
        UnloadGuard::Block("screen telemetry trampoline still had a live thread at detach");
        return;
    }
    (void)HookManager::Remove(listenerTarget);
    listenerTarget = nullptr;
    originalListener = nullptr;
}

bool Known() noexcept { return known.load(std::memory_order_acquire); }
bool OnTitleScreen() noexcept { return onTitle.load(std::memory_order_acquire); }
bool OnDisconnectedScreen() noexcept { return onDisconnected.load(std::memory_order_acquire); }
bool LastLoadWasLocalWorld() noexcept { return localWorldLoad.load(std::memory_order_acquire); }

}
