#include "Oinkster/MouseInputBlocker.hpp"
#include "Oinkster/Capabilities.hpp"
#include "Oinkster/Diagnostics.hpp"
#include "Oinkster/Dx12Overlay.hpp"
#include "Oinkster/Module.hpp"
#include "Oinkster/ModuleManager.hpp"
#include <Windows.h>
#include <GameInput.h>
#include <MinHook.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <format>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <array>
#include <vector>

namespace Oinkster::MouseInputBlocker {
namespace {
using GameInputCreateFunction = HRESULT(WINAPI*)(void**);
using GetCurrentReadingFunction = HRESULT(STDMETHODCALLTYPE*)(void*, std::uint32_t, void*, void**);
using GetMouseStateFunction = bool(STDMETHODCALLTYPE*)(void*, GameInputMouseState*);
using MouseFeedFunction = void(__fastcall*)(void*, std::int8_t, std::int8_t, std::int16_t,
    std::int16_t, std::int16_t, std::int16_t, std::int8_t);

constexpr IID GameInputV2Iid{0xbbaa66d2, 0x837a, 0x40f7, {0xa3, 0x03, 0x91, 0x7d, 0x50, 0x09, 0x55, 0xf4}};
constexpr std::size_t GetCurrentReadingVtableIndex = 4;
// IGameInputReading inherits the three IUnknown slots. GetMouseState follows
// GetKeyCount/GetKeyState and is slot 16 in the Windows SDK interface. Slot
// 14 is GetKeyCount; hooking it as a mouse method silently misses all mouse
// input and invokes the detour with the wrong ABI.
constexpr std::size_t GetMouseStateVtableIndex = 16;
constexpr auto DiscoveryTimeout = std::chrono::seconds(10);

std::atomic<InstallState> state{InstallState::Unavailable};
std::atomic_bool active{};
std::atomic_uint32_t activeCallbacks{};
std::mutex installMutex;
std::mutex transformMutex;
GetMouseStateFunction originalGetMouseState{};
void* hookTarget{};
void* internalHookTarget{};
MouseFeedFunction originalMouseFeed{};
std::atomic_bool internalWasActive{};
std::atomic_bool buttonFilteringReady{};
std::atomic_bool suppressedLeftRelease{};
std::atomic_bool suppressedRightRelease{};
std::atomic_bool forwardedLeftDown{};
std::atomic_bool forwardedRightDown{};
std::atomic_bool replayHotbar{};
// A left/right press the replay hotbar took; its release must not reach
// the game.
std::atomic_bool hotbarSwallowedLeftDown{};
std::atomic_bool hotbarSwallowedRightDown{};
std::atomic<ModuleManager*> moduleManager{};
std::chrono::steady_clock::time_point discoveryDeadline{};

std::atomic_bool lookCapture{};
bool havePosition{};
bool wasActive{};
bool wasCapturing{};
std::int64_t captureLastX{};
std::int64_t captureLastY{};
std::int64_t capturedX{};
std::int64_t capturedY{};
std::int64_t offsetX{};
std::int64_t offsetY{};
std::int64_t frozenX{};
std::int64_t frozenY{};

std::atomic_bool blockWheel{};
std::atomic<float> sensitivityX{1.0F};
std::atomic<float> sensitivityY{1.0F};
// Sensitivity: the reported position integrates scaled movement, so any
// multiplier change (or 1.0) continues smoothly from where it was.
bool haveScaled{};
std::int64_t scaledInputX{};
std::int64_t scaledInputY{};
double scaledX{};
double scaledY{};

// Applied last to whatever position the reading reports (transformMutex held).
void ApplySensitivity(GameInputMouseState* mouse) noexcept {
    if (!haveScaled) {
        scaledInputX = mouse->positionX;
        scaledInputY = mouse->positionY;
        scaledX = static_cast<double>(mouse->positionX);
        scaledY = static_cast<double>(mouse->positionY);
        haveScaled = true;
    }
    scaledX += static_cast<double>(mouse->positionX - scaledInputX) * sensitivityX.load(std::memory_order_relaxed);
    scaledY += static_cast<double>(mouse->positionY - scaledInputY) * sensitivityY.load(std::memory_order_relaxed);
    scaledInputX = mouse->positionX;
    scaledInputY = mouse->positionY;
    mouse->positionX = static_cast<std::int64_t>(std::llround(scaledX));
    mouse->positionY = static_cast<std::int64_t>(std::llround(scaledY));
}

void DumpLoadedMinecraftImage(HMODULE module) noexcept {
#if defined(_DEBUG)
    try {
        const auto* base = reinterpret_cast<const std::byte*>(module);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (!module || dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) return;
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return;
        const auto* sections = IMAGE_FIRST_SECTION(nt);
        std::size_t fileSize = nt->OptionalHeader.SizeOfHeaders;
        for (WORD index = 0; index < nt->FileHeader.NumberOfSections; ++index) {
            fileSize = std::max(fileSize, static_cast<std::size_t>(sections[index].PointerToRawData)
                + static_cast<std::size_t>(sections[index].SizeOfRawData));
        }
        std::vector<std::byte> image(fileSize);
        std::memcpy(image.data(), base, nt->OptionalHeader.SizeOfHeaders);
        for (WORD index = 0; index < nt->FileHeader.NumberOfSections; ++index) {
            const auto copySize = std::min<std::size_t>(sections[index].Misc.VirtualSize, sections[index].SizeOfRawData);
            if (copySize == 0) continue;
            std::memcpy(image.data() + sections[index].PointerToRawData,
                base + sections[index].VirtualAddress, copySize);
        }
        wchar_t localAppData[MAX_PATH]{};
        const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH);
        const auto root = length > 0 && length < MAX_PATH
            ? std::filesystem::path(localAppData) / "Oinkster" / "Client" / "analysis"
            : std::filesystem::temp_directory_path() / "Oinkster" / "analysis";
        std::filesystem::create_directories(root);
        const auto path = root / "Minecraft.Windows-1.26.5203.0-memory-image.exe";
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(image.data()), static_cast<std::streamsize>(image.size()));
        output.close();
        Diagnostics::LogLine("info", "analysis_image", std::format(
            "Wrote debug analysis image size={} path={}", image.size(), path.string()));
    } catch (...) {
        Diagnostics::LogLine("warning", "analysis_image", "Could not write the debug analysis image");
    }
#else
    (void)module;
#endif
}

template<std::size_t Size>
std::vector<const std::byte*> FindPattern(HMODULE module, const std::array<int, Size>& pattern) {
    std::vector<const std::byte*> matches;
    const auto* base = reinterpret_cast<const std::byte*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (!module || dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) return matches;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return matches;
    const auto* section = IMAGE_FIRST_SECTION(nt);
    for (WORD sectionIndex = 0; sectionIndex < nt->FileHeader.NumberOfSections; ++sectionIndex) {
        if ((section[sectionIndex].Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0) continue;
        const auto* start = base + section[sectionIndex].VirtualAddress;
        const auto length = static_cast<std::size_t>(section[sectionIndex].Misc.VirtualSize);
        if (length < Size) continue;
        for (std::size_t offset = 0; offset <= length - Size; ++offset) {
            bool found = true;
            for (std::size_t index = 0; index < Size; ++index) {
                if (pattern[index] >= 0 && std::to_integer<unsigned char>(start[offset + index]) != pattern[index]) {
                    found = false;
                    break;
                }
            }
            if (found) matches.push_back(start + offset);
        }
    }
    return matches;
}

class CallbackGuard final {
public:
    CallbackGuard() noexcept { activeCallbacks.fetch_add(1, std::memory_order_acq_rel); }
    ~CallbackGuard() { activeCallbacks.fetch_sub(1, std::memory_order_acq_rel); }
};

bool IsExecutableAddressInModule(HMODULE module, const void* address) noexcept {
    if (!module || !address) return false;
    const auto* base = reinterpret_cast<const std::byte*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    const auto target = reinterpret_cast<std::uintptr_t>(address);
    const auto imageBase = reinterpret_cast<std::uintptr_t>(base);
    if (target < imageBase) return false;
    const auto rva = target - imageBase;
    const auto* section = IMAGE_FIRST_SECTION(nt);
    for (WORD index = 0; index < nt->FileHeader.NumberOfSections; ++index) {
        const auto start = static_cast<std::uintptr_t>(section[index].VirtualAddress);
        const auto size = static_cast<std::uintptr_t>(section[index].Misc.VirtualSize);
        if (rva >= start && rva < start + size) {
            return (section[index].Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0;
        }
    }
    return false;
}

bool STDMETHODCALLTYPE HookGetMouseState(void* reading, GameInputMouseState* mouse) {
    const CallbackGuard guard;
    // Test `mouse` BEFORE handing it to the original: the null check used to
    // sit last in this chain, so a null state pointer was passed through first
    // and only rejected afterwards.
    if (!mouse || !originalGetMouseState || !originalGetMouseState(reading, mouse)) return false;

    const bool shouldBlock = active.load(std::memory_order_acquire);
    std::scoped_lock lock(transformMutex);
    const auto logicalX = mouse->positionX - offsetX;
    const auto logicalY = mouse->positionY - offsetY;
    if (!havePosition) {
        frozenX = logicalX;
        frozenY = logicalY;
        havePosition = true;
    }

    if (shouldBlock) {
        if (!wasActive) {
            frozenX = logicalX;
            frozenY = logicalY;
            wasActive = true;
        }
        mouse->positionX = frozenX;
        mouse->positionY = frozenY;
        mouse->buttons = static_cast<GameInputMouseButtons>(0);
        mouse->wheelX = 0;
        mouse->wheelY = 0;
        ApplySensitivity(mouse);
        return true;
    }

    if (lookCapture.load(std::memory_order_acquire)) {
        if (!wasCapturing) {
            frozenX = logicalX;
            frozenY = logicalY;
            captureLastX = logicalX;
            captureLastY = logicalY;
            wasCapturing = true;
        }
        capturedX += logicalX - captureLastX;
        capturedY += logicalY - captureLastY;
        captureLastX = logicalX;
        captureLastY = logicalY;
        mouse->positionX = frozenX;
        mouse->positionY = frozenY;
        ApplySensitivity(mouse);
        return true;
    }

    if (wasCapturing) {
        // Resume from the frozen position so the player does not jump.
        offsetX = mouse->positionX - frozenX;
        offsetY = mouse->positionY - frozenY;
        mouse->positionX = frozenX;
        mouse->positionY = frozenY;
        wasCapturing = false;
        ApplySensitivity(mouse);
        return true;
    }

    if (wasActive) {
        offsetX = mouse->positionX - frozenX;
        offsetY = mouse->positionY - frozenY;
        mouse->positionX = frozenX;
        mouse->positionY = frozenY;
        mouse->buttons = static_cast<GameInputMouseButtons>(0);
        mouse->wheelX = 0;
        mouse->wheelY = 0;
        wasActive = false;
        ApplySensitivity(mouse);
        return true;
    }

    mouse->positionX = logicalX;
    mouse->positionY = logicalY;
    if (blockWheel.load(std::memory_order_relaxed)) {
        mouse->wheelX = 0;
        mouse->wheelY = 0;
    }
    ApplySensitivity(mouse);
    return true;
}

void __fastcall HookMouseFeed(void* mouseDevice, std::int8_t type, std::int8_t action,
    std::int16_t mouseX, std::int16_t mouseY, std::int16_t movementX,
    std::int16_t movementY, std::int8_t extra) {
    const CallbackGuard guard;
    if (active.load(std::memory_order_acquire)) {
        if (!internalWasActive.exchange(true, std::memory_order_acq_rel) && originalMouseFeed) {
            originalMouseFeed(mouseDevice, 1, 0, mouseX, mouseY, 0, 0, extra);
            originalMouseFeed(mouseDevice, 2, 0, mouseX, mouseY, 0, 0, extra);
            forwardedLeftDown.store(false, std::memory_order_release);
            forwardedRightDown.store(false, std::memory_order_release);
            suppressedLeftRelease.store(false, std::memory_order_release);
            suppressedRightRelease.store(false, std::memory_order_release);
        }
        if (type == 4) {
            const auto rawAction = static_cast<std::uint8_t>(action);
            if (rawAction == 0x78 || rawAction == 0x7f) Dx12Overlay::QueueMouseWheel(1.0F);
            else if (rawAction == 0x80 || rawAction == 0x88) Dx12Overlay::QueueMouseWheel(-1.0F);
        }
        return;
    }
    internalWasActive.store(false, std::memory_order_release);

    // Replay hotbar: wheel notches pick a slot and a left click uses it.
    if (type == 1 && action == 0 && hotbarSwallowedLeftDown.exchange(false, std::memory_order_acq_rel)) return;
    if (type == 2 && action == 0 && hotbarSwallowedRightDown.exchange(false, std::memory_order_acq_rel)) return;
    if (replayHotbar.load(std::memory_order_acquire)) {
        if (type == 4) {
            const auto rawAction = static_cast<std::uint8_t>(action);
            if (rawAction == 0x78 || rawAction == 0x7f) Dx12Overlay::QueueReplayHotbarScroll(1);
            else if (rawAction == 0x80 || rawAction == 0x88) Dx12Overlay::QueueReplayHotbarScroll(-1);
            return;
        }
        if (type == 1 && action != 0 && !forwardedLeftDown.load(std::memory_order_acquire)) {
            hotbarSwallowedLeftDown.store(true, std::memory_order_release);
            Dx12Overlay::QueueReplayHotbarClick();
            return;
        }
        if (type == 2 && action != 0 && !forwardedRightDown.load(std::memory_order_acquire)) {
            hotbarSwallowedRightDown.store(true, std::memory_order_release);
            Dx12Overlay::QueueReplayHotbarRightClick();
            return;
        }
    }

    // Scroll wheel disabler: the hotbar never sees the notch.
    if (type == 4 && blockWheel.load(std::memory_order_relaxed)) return;

    // MouseDevice::feed types 1/2 are left/right button transitions and
    // action 0/1 is up/down. This is the authoritative producer shared by
    // GameInput and Win32 raw input on the reviewed build. Publish before the
    // game receives the packet so Immediate modules can cancel it; later
    // modules still observe the same event as raw telemetry.
    if ((type == 1 || type == 2) && buttonFilteringReady.load(std::memory_order_acquire)) {
        const std::uint32_t button = type == 1 ? 1U : 2U;
        const bool pressed = action != 0;
        auto& pairedRelease = type == 1 ? suppressedLeftRelease : suppressedRightRelease;
        auto& forwardedDown = type == 1 ? forwardedLeftDown : forwardedRightDown;
        if (!pressed && pairedRelease.exchange(false, std::memory_order_acq_rel)) return;
        if (auto* modules = moduleManager.load(std::memory_order_acquire)) {
            const MouseEvent event{static_cast<float>(mouseX), static_cast<float>(mouseY), 0.0F,
                pressed ? button : 0U, button, pressed, false, SteadyClock::now()};
            if (!modules->PublishInput(event)) {
                // Suppress the matching release only when gameplay never saw
                // a down. If a device emits a duplicate down while already
                // held, its eventual up must still be forwarded or the game
                // would be left with a stuck attack/use button.
                if (pressed && !forwardedDown.load(std::memory_order_acquire)) {
                    pairedRelease.store(true, std::memory_order_release);
                }
                return;
            }
        }
        forwardedDown.store(pressed, std::memory_order_release);
    }
    if (originalMouseFeed) {
        originalMouseFeed(mouseDevice, type, action, mouseX, mouseY, movementX, movementY, extra);
    }
}

bool TryInstallInternalQueueHook(std::wstring& detail) noexcept {
    const HMODULE minecraft = GetModuleHandleW(nullptr);
    // Exact 1.26.5203.0 MouseDevice::feed prologue. This is the producer that
    // appends the 20-byte mouse packets consumed by the game, so blocking it
    // covers both GameInput and Win32 raw-input sources before gameplay sees
    // movement, buttons, or wheel events.
    constexpr std::array feedPattern{
        0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x56, 0x57, 0x55, 0x53,
        0x48, 0x83, 0xEC, 0x48, 0x44, 0x89, 0xCF, 0x44, 0x89, 0xC3, 0x89, 0xD5,
        0x48, 0x89, 0xCE, 0x44, 0x0F, 0xB7, 0xA4, 0x24, 0xC0, 0x00, 0x00, 0x00,
        0x44, 0x0F, 0xB7, 0xAC, 0x24, 0xB8, 0x00, 0x00, 0x00};
    const auto feedMatches = FindPattern(minecraft, feedPattern);
    Diagnostics::LogLine("info", "internal_mouse_scan", std::format(
        "Minecraft 1.26 MouseDevice::feed matches={}", feedMatches.size()));
    if (feedMatches.size() != 1) {
        DumpLoadedMinecraftImage(minecraft);
        detail = L"The exact 1.26 MouseDevice::feed signature was missing or ambiguous.";
        return false;
    }

    auto* feedTarget = const_cast<std::byte*>(feedMatches.front());
    if (!IsExecutableAddressInModule(minecraft, feedTarget)) {
        detail = L"The resolved 1.26 mouse feed failed section validation.";
        return false;
    }

    const auto createResult = MH_CreateHook(feedTarget, reinterpret_cast<void*>(HookMouseFeed),
        reinterpret_cast<void**>(&originalMouseFeed));
    if (createResult != MH_OK || MH_EnableHook(feedTarget) != MH_OK) {
        if (createResult == MH_OK) (void)MH_RemoveHook(feedTarget);
        originalMouseFeed = nullptr;
        detail = L"The validated 1.26 mouse feed hook could not be enabled.";
        return false;
    }

    internalHookTarget = feedTarget;
    buttonFilteringReady.store(true, std::memory_order_release);
    detail = L"GameInput and the exact 1.26 mouse feed are guarded.";
    Diagnostics::LogLine("info", "internal_mouse_scan", "Validated Minecraft 1.26 MouseDevice::feed hook installed");
    return true;
}

InstallState RetryOrFail(std::wstring& detail, std::wstring_view message) noexcept {
    detail.assign(message);
    if (std::chrono::steady_clock::now() < discoveryDeadline) return InstallState::Initializing;
    state.store(InstallState::Unavailable, std::memory_order_release);
    if (auto* modules = moduleManager.load(std::memory_order_acquire)) {
        try {
            modules->SetCapability(Capability::InputFiltering, {CapabilityState::Unavailable,
                L"The reviewed mouse input path did not become available: " + detail});
        } catch (...) {}
    }
    return InstallState::Unavailable;
}
}

bool Install(ModuleManager& modules, std::wstring& detail) noexcept {
    std::scoped_lock lock(installMutex);
    moduleManager.store(&modules, std::memory_order_release);
    buttonFilteringReady.store(false, std::memory_order_release);
    suppressedLeftRelease.store(false, std::memory_order_release);
    suppressedRightRelease.store(false, std::memory_order_release);
    forwardedLeftDown.store(false, std::memory_order_release);
    forwardedRightDown.store(false, std::memory_order_release);
    active.store(false, std::memory_order_release);
    discoveryDeadline = std::chrono::steady_clock::now() + DiscoveryTimeout;
    state.store(InstallState::Initializing, std::memory_order_release);
    try {
        modules.SetCapability(Capability::InputFiltering, {CapabilityState::Pending,
            L"Waiting for the reviewed Minecraft mouse queue hook to validate."});
    } catch (...) {}
    detail = L"GameInput mouse discovery is initializing.";
    return true;
}

InstallState PollInstall(std::wstring& detail) noexcept {
    if (state.load(std::memory_order_acquire) != InstallState::Initializing) return state.load();
    std::scoped_lock lock(installMutex);
    if (state.load(std::memory_order_acquire) != InstallState::Initializing) return state.load();

    const HMODULE module = GetModuleHandleW(L"GameInputRedist.dll");
    if (!module) return RetryOrFail(detail, L"GameInputRedist.dll is not loaded");
    const auto create = reinterpret_cast<GameInputCreateFunction>(GetProcAddress(module, "GameInputCreate"));
    if (!create) return RetryOrFail(detail, L"GameInputCreate is unavailable");

    void* legacy{};
    if (FAILED(create(&legacy)) || !legacy) return RetryOrFail(detail, L"GameInput creation failed");
    void* gameInput{};
    const HRESULT queryResult = reinterpret_cast<IUnknown*>(legacy)->QueryInterface(GameInputV2Iid, &gameInput);
    reinterpret_cast<IUnknown*>(legacy)->Release();
    if (FAILED(queryResult) || !gameInput) return RetryOrFail(detail, L"GameInput v2 is unavailable");

    auto** inputVtable = *reinterpret_cast<void***>(gameInput);
    const auto getCurrentReading = inputVtable
        ? reinterpret_cast<GetCurrentReadingFunction>(inputVtable[GetCurrentReadingVtableIndex]) : nullptr;
    void* reading{};
    if (!getCurrentReading || FAILED(getCurrentReading(gameInput, 0x20U, nullptr, &reading)) || !reading) {
        reinterpret_cast<IUnknown*>(gameInput)->Release();
        return RetryOrFail(detail, L"A current GameInput mouse reading is not available");
    }

    auto** readingVtable = *reinterpret_cast<void***>(reading);
    void* target = readingVtable ? readingVtable[GetMouseStateVtableIndex] : nullptr;
    if (!IsExecutableAddressInModule(module, target)) {
        reinterpret_cast<IUnknown*>(reading)->Release();
        reinterpret_cast<IUnknown*>(gameInput)->Release();
        return RetryOrFail(detail, L"GameInput GetMouseState validation failed");
    }

    const auto createResult = MH_CreateHook(target, reinterpret_cast<void*>(HookGetMouseState),
        reinterpret_cast<void**>(&originalGetMouseState));
    if (createResult != MH_OK || MH_EnableHook(target) != MH_OK) {
        if (createResult == MH_OK) (void)MH_RemoveHook(target);
        originalGetMouseState = nullptr;
        reinterpret_cast<IUnknown*>(reading)->Release();
        reinterpret_cast<IUnknown*>(gameInput)->Release();
        return RetryOrFail(detail, L"GameInput GetMouseState hook failed");
    }

    hookTarget = target;
    reinterpret_cast<IUnknown*>(reading)->Release();
    reinterpret_cast<IUnknown*>(gameInput)->Release();
    std::wstring internalDetail;
    const bool internalReady = TryInstallInternalQueueHook(internalDetail);
    Diagnostics::LogLine(internalReady ? "info" : "warning", "internal_mouse_hook",
        internalReady ? "Validated Minecraft 1.26 MouseDevice::feed hook installed"
            : "Minecraft 1.26 mouse feed signature unavailable; see internal_mouse_scan counts");
    state.store(InstallState::Ready, std::memory_order_release);
    if (auto* modules = moduleManager.load(std::memory_order_acquire)) {
        try {
            modules->SetCapability(Capability::InputFiltering, internalReady
                ? CapabilityStatus{CapabilityState::Available, {}}
                : CapabilityStatus{CapabilityState::Unavailable,
                    L"The reviewed Minecraft mouse queue signature did not validate; button filtering is disabled."});
        } catch (...) {}
    }
    detail = internalReady ? internalDetail : L"GameInput is guarded; the internal 1.26 mouse queue signature was unavailable.";
    Diagnostics::LogLine("info", "mouse_input_blocker", "GameInput GetMouseState hook installed");
    return InstallState::Ready;
}

InstallState State() noexcept { return state.load(std::memory_order_acquire); }

bool CanFilterButtons() noexcept { return buttonFilteringReady.load(std::memory_order_acquire); }

void SetActive(bool value) noexcept { active.store(value, std::memory_order_release); }

void SetReplayHotbar(bool enabled) noexcept { replayHotbar.store(enabled, std::memory_order_release); }

bool ReplayHotbarCaptured() noexcept { return internalHookTarget != nullptr && originalMouseFeed != nullptr; }

void SetLookCapture(bool capture) noexcept { lookCapture.store(capture, std::memory_order_release); }

void SetMouseShaping(const MouseShaping& shaping) noexcept {
    blockWheel.store(shaping.blockWheel, std::memory_order_relaxed);
    const auto clampScale = [](float value) { return std::isfinite(value) ? std::clamp(value, 0.05F, 5.0F) : 1.0F; };
    sensitivityX.store(clampScale(shaping.sensitivityX), std::memory_order_relaxed);
    sensitivityY.store(clampScale(shaping.sensitivityY), std::memory_order_relaxed);
}

void TakeLookDelta(std::int64_t& deltaX, std::int64_t& deltaY) noexcept {
    std::scoped_lock lock(transformMutex);
    deltaX = capturedX;
    deltaY = capturedY;
    capturedX = 0;
    capturedY = 0;
}

void Remove() noexcept {
    active.store(false, std::memory_order_release);
    replayHotbar.store(false, std::memory_order_release);
    std::scoped_lock installLock(installMutex);
    state.store(InstallState::Unavailable, std::memory_order_release);
    if (hookTarget) (void)MH_DisableHook(hookTarget);
    if (internalHookTarget) (void)MH_DisableHook(internalHookTarget);
    for (unsigned attempt = 0; activeCallbacks.load(std::memory_order_acquire) != 0 && attempt < 100; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (hookTarget) (void)MH_RemoveHook(hookTarget);
    if (internalHookTarget) (void)MH_RemoveHook(internalHookTarget);
    hookTarget = nullptr;
    internalHookTarget = nullptr;
    originalGetMouseState = nullptr;
    originalMouseFeed = nullptr;
    buttonFilteringReady.store(false, std::memory_order_release);
    suppressedLeftRelease.store(false, std::memory_order_release);
    suppressedRightRelease.store(false, std::memory_order_release);
    forwardedLeftDown.store(false, std::memory_order_release);
    forwardedRightDown.store(false, std::memory_order_release);
    internalWasActive.store(false, std::memory_order_release);
    if (auto* modules = moduleManager.exchange(nullptr, std::memory_order_acq_rel)) {
        try {
            modules->SetCapability(Capability::InputFiltering,
                {CapabilityState::Unavailable, L"Mouse input filtering was detached."});
        } catch (...) {}
    }
    blockWheel.store(false, std::memory_order_relaxed);
    sensitivityX.store(1.0F, std::memory_order_relaxed);
    sensitivityY.store(1.0F, std::memory_order_relaxed);
    std::scoped_lock transformLock(transformMutex);
    haveScaled = false;
    havePosition = false;
    wasActive = false;
    offsetX = 0;
    offsetY = 0;
    frozenX = 0;
    frozenY = 0;
}
}
