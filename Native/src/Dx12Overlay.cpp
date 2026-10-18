#include "Oinkster/Dx12Overlay.hpp"
#include "Oinkster/FrameBlend.hpp"
#include "Oinkster/PostProcess.hpp"
#include "Oinkster/VideoRecorder.hpp"
#include "Oinkster/ModuleManager.hpp"
#include "Oinkster/OverlayUi.hpp"
#include "Oinkster/InputState.hpp"
#include "Oinkster/MouseInputBlocker.hpp"
#include "Oinkster/PlayerSkinTelemetry.hpp"
#include "Oinkster/Diagnostics.hpp"
#include "Oinkster/HookManager.hpp"
#include "Oinkster/UnloadGuard.hpp"
#include "Oinkster/ReplayInjector.hpp"
#include "Oinkster/ReplayWorld.hpp"
#include "Oinkster/ScreenTelemetry.hpp"
#include "Oinkster/WorldTelemetry.hpp"
#include "Oinkster/Modules/AccessibilityLogic.hpp"
#include <Windows.h>
#include <windowsx.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <MinHook.h>
#include <imgui.h>
#include <imgui_impl_dx12.h>
#include <imgui_impl_win32.h>
#include <wrl/client.h>
#include <winrt/Windows.UI.Core.h>
#include <algorithm>
#include <atomic>
#include <array>
#include <bitset>
#include <cctype>
#include <cfloat>
#include <cstddef>
#include <cstring>
#include <cmath>
#include <chrono>
#include <format>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include "../resources/resource.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

namespace Oinkster::Dx12Overlay {
namespace {
using Microsoft::WRL::ComPtr;
using PresentFunction = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using ResizeFunction = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
using Resize1Function = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*, UINT, UINT, UINT, DXGI_FORMAT, UINT,
    const UINT*, IUnknown* const*);
using ExecuteFunction = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
using ClipCursorFunction = BOOL(WINAPI*)(const RECT*);
using GetRawInputDataFunction = UINT(WINAPI*)(HRAWINPUT, UINT, LPVOID, PUINT, UINT);
using GetRawInputBufferFunction = UINT(WINAPI*)(PRAWINPUT, PUINT, UINT);

struct FrameContext {
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12Resource> backBuffer;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
    UINT64 fenceValue{};
};

PresentFunction originalPresent{};
ResizeFunction originalResize{};
Resize1Function originalResize1{};
ExecuteFunction originalExecute{};
ClipCursorFunction originalClipCursor{};
GetRawInputDataFunction originalGetRawInputData{};
GetRawInputBufferFunction originalGetRawInputBuffer{};
void** rawInputDataImportSlot{};
std::atomic<ID3D12CommandQueue*> commandQueue{};
ComPtr<ID3D12Device> device;
ComPtr<ID3D12GraphicsCommandList> commandList;
ComPtr<ID3D12DescriptorHeap> rtvHeap;
ComPtr<ID3D12DescriptorHeap> srvHeap;
ComPtr<ID3D12Fence> renderFence;
std::vector<FrameContext> frames;
HANDLE fenceEvent{};
UINT64 nextFenceValue{1};
HWND gameWindow{};
WNDPROC originalWindowProcedure{};
DXGI_FORMAT renderFormat{DXGI_FORMAT_UNKNOWN};
std::mutex stateMutex;
std::atomic_bool installed{};
std::atomic_bool imguiReady{};
std::atomic_bool menuVisible{};
bool hudEditMode{};
// Render-thread state for frame timing and pacing.
std::uint64_t frameCounter{};
std::chrono::steady_clock::time_point lastPresentAt{};
// Signalled by Remove() so a dynamic-FPS wait in Present ends immediately.
HANDLE pacingWakeEvent{};
FrameBlendResources frameBlend;
PostProcessPass postProcess;
// The grade the post-process draw callback binds (render thread only).
PostProcessSettings framePostProcess;
// Gameplay owns the mouse and keyboard (no game screen, no menu, focused):
// written by HookPresent, read by WindowProcedure for key transforms.
std::atomic_bool gameplayCaptured{};
bool lastGameplayCaptured{};
VideoRecorder::GpuCapture videoCapture;
constexpr UINT FontDescriptorCount = 1;
constexpr UINT SrvDescriptorCount = FontDescriptorCount + FrameBlendResources::HistoryCount;
ModuleManager* moduleManager{};
// Tracks which skin revision has already been handed to OverlayUi, purely so
// a repeated delivery of the same revision isn't logged twice.
std::atomic_uint64_t playerHeadRevision{};
std::atomic_bool settingsDirty{};
std::atomic_bool ejectRequested{};
std::atomic_uint32_t configuredMenuKey{'K'};
std::atomic_uint32_t configuredUnloadKey{VK_OEM_5};
std::atomic_uint32_t activeCallbacks{};
KeyStateTracker keyStateTracker;
// keyStateTracker is touched from the window thread (WindowProcedure) and the
// render thread (ReleaseTrackedKeys/RestoreTrackedKeys via HookPresent).
std::mutex keyStateMutex;
// Dear ImGui is not thread-safe. Its input queue (an ImVector) is filled by
// ImGui_ImplWin32_WndProcHandler on the game's window thread and drained by
// ImGui::NewFrame on the render thread; unsynchronized, the two race and trip
// ImVector's bounds assert. Every ImGui call made off the render thread, and
// the render thread's input/NewFrame step, holds this. Recursive because the
// Win32 handler's SetCapture/ReleaseCapture re-enter WindowProcedure
// synchronously on the same thread.
std::recursive_mutex imguiInputMutex;
std::atomic_uint32_t rawInputMessagesWhileMenu{};
std::atomic_uint32_t mouseMessagesWhileMenu{};
std::atomic_uint32_t keyboardMessagesWhileMenu{};
std::chrono::steady_clock::time_point lastDiagnosticLog{};
std::atomic_uint32_t extraWindowMouseMessages{};
std::atomic_uint32_t extraWindowKeyMessages{};
std::atomic_uint32_t rawMousePacketsBlocked{};
std::atomic_int extraVerticalWheel{};
std::atomic_int extraHorizontalWheel{};
// Keys currently held on the player's behalf by scoped assist modules.
// Remembered so settings changes, focus loss, and unload always emit key-up.
std::vector<std::uint32_t> heldAssistKeys;
bool menuKeyDown{};
bool unloadKeyDown{};
bool escapeKeyDown{};
bool cursorVisibilityInitialized{};
bool observedCursorVisible{};
bool stableCursorVisible{};
std::chrono::steady_clock::time_point cursorVisibilityChangedAt{};
std::atomic_bool pauseProbeRequested{};
std::atomic_bool overlayControlsVisible{};
// The open menu is the standalone Replays theater (opened from the main-menu
// button). The game's cursor is already free there, so opening and closing it
// must not release or re-grab the cursor the way the ClickGUI does.
std::atomic_bool menuIsReplayTheater{};
// The menu is open as the replay editor (opened when a replay world is
// joined, or with K while one plays) rather than the ClickGUI. Cursor and
// input handling are the ClickGUI's; only what OverlayUi draws differs.
std::atomic_bool menuIsReplayEditor{};
// The editor already opened itself for the replay that is running.
bool replayEditorAutoOpened{};
// Set only after the replay's local world has rendered at least one frame.
// This distinguishes a real Save & Quit from transient title events emitted
// while that local world is still starting.
bool replayWorldRendered{};
// While a replay plays (no menu, no pause screen) the wheel and left click
// drive the replay hotbar instead of the game. Queued by the mouse feed hook
// (or, without it, the window procedure / button polling) and drained by
// the next Present.
std::atomic_bool replayHotbarActive{};
std::atomic_int replayHotbarScroll{};
std::atomic_int replayHotbarClicks{};
std::atomic_int replayHotbarRightClicks{};
bool replayHotbarLeftDown{};
bool replayHotbarRightDown{};
// The temporary replay world is deleted once the game is back on its menus
// after a replay (and once after injection, for one left by a crash).
bool replayWorldCleanupDue{true};
// Present-thread only: last logged in-world state, for transition logging.
bool loggedInWorld{true};
// Present-thread only: GameInput mouse is blocked because the pointer is over
// one of the small overlay controls (not because the menu is open).
bool controlsBlockingMouse{};
bool minecraftPauseVisible{};
std::chrono::steady_clock::time_point pauseCandidateUntil{};
std::chrono::steady_clock::time_point pauseCursorHiddenSince{};
bool previousFrameCursorVisible{};
// Cursor state the game had when the ClickGUI opened, put back exactly on
// close. Render thread only (ShowCursor's display count is per thread).
int showCursorCountBeforeMenu{};
bool cursorHiddenBeforeMenu{};
bool cursorStateSaved{};
// Focus loss closes the menu on the window thread; the render thread puts
// the saved cursor state back on its next Present.
std::atomic_bool cursorRestorePending{};

bool IsCursorVisible() noexcept;

UINT WINAPI HookGetRawInputData(HRAWINPUT input, UINT command, LPVOID data, PUINT size, UINT headerSize) noexcept;

void** FindMainModuleImportSlot(const char* functionName) noexcept {
    const auto module = reinterpret_cast<std::byte*>(GetModuleHandleW(nullptr));
    if (!module || !functionName) return nullptr;

    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) return nullptr;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(module + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return nullptr;

    const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (directory.VirtualAddress == 0 || directory.Size < sizeof(IMAGE_IMPORT_DESCRIPTOR)) return nullptr;
    auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(module + directory.VirtualAddress);
    for (; descriptor->Name != 0; ++descriptor) {
        const DWORD lookupRva = descriptor->OriginalFirstThunk != 0
            ? descriptor->OriginalFirstThunk : descriptor->FirstThunk;
        auto* lookup = reinterpret_cast<IMAGE_THUNK_DATA64*>(module + lookupRva);
        auto* address = reinterpret_cast<IMAGE_THUNK_DATA64*>(module + descriptor->FirstThunk);
        for (; lookup->u1.AddressOfData != 0; ++lookup, ++address) {
            if (IMAGE_SNAP_BY_ORDINAL64(lookup->u1.Ordinal)) continue;
            const auto* import = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(module + lookup->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char*>(import->Name), functionName) == 0) {
                return reinterpret_cast<void**>(&address->u1.Function);
            }
        }
    }
    return nullptr;
}

bool InstallRawInputDataImportHook() noexcept {
    rawInputDataImportSlot = FindMainModuleImportSlot("GetRawInputData");
    if (!rawInputDataImportSlot || !*rawInputDataImportSlot) return false;
    originalGetRawInputData = reinterpret_cast<GetRawInputDataFunction>(*rawInputDataImportSlot);

    DWORD previousProtection{};
    if (!VirtualProtect(rawInputDataImportSlot, sizeof(void*), PAGE_READWRITE, &previousProtection)) {
        originalGetRawInputData = nullptr;
        rawInputDataImportSlot = nullptr;
        return false;
    }
    InterlockedExchangePointer(reinterpret_cast<PVOID volatile*>(rawInputDataImportSlot),
        reinterpret_cast<void*>(&HookGetRawInputData));
    DWORD ignoredProtection{};
    (void)VirtualProtect(rawInputDataImportSlot, sizeof(void*), previousProtection, &ignoredProtection);
    Diagnostics::LogLine("info", "raw_input_hook", "Minecraft GetRawInputData import slot patched");
    return true;
}

void RestoreRawInputDataImport() noexcept {
    if (!rawInputDataImportSlot || !originalGetRawInputData) return;
    DWORD previousProtection{};
    if (VirtualProtect(rawInputDataImportSlot, sizeof(void*), PAGE_READWRITE, &previousProtection)) {
        if (*rawInputDataImportSlot == reinterpret_cast<void*>(&HookGetRawInputData)) {
            InterlockedExchangePointer(reinterpret_cast<PVOID volatile*>(rawInputDataImportSlot),
                reinterpret_cast<void*>(originalGetRawInputData));
        }
        DWORD ignoredProtection{};
        (void)VirtualProtect(rawInputDataImportSlot, sizeof(void*), previousProtection, &ignoredProtection);
    }
    rawInputDataImportSlot = nullptr;
}

// Minecraft (a UWP app) can present through one HWND (what DXGI's swap chain
// reports as OutputWindow, subclassed below as gameWindow) while a SEPARATE
// HWND - e.g. a CoreWindow - actually receives raw mouse/keyboard input.
// Subclassing only gameWindow then lets input slip straight past our menu
// (confirmed: scroll wheel changes the hotbar while the menu is open, which
// only happens if Minecraft's input window never saw our block). To cover
// that, every top-level window this process owns gets subclassed too, purely
// to swallow mouse/keyboard/WM_INPUT while the menu is visible.
std::mutex extraProceduresMutex;
std::unordered_map<HWND, WNDPROC> extraOriginalProcedures;

LRESULT CALLBACK ExtraInputBlockingProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (menuVisible) {
        const bool mouseRange = message >= WM_MOUSEFIRST && message <= WM_MOUSELAST;
        const bool keyRange = message >= WM_KEYFIRST && message <= WM_KEYLAST;
        if (mouseRange) extraWindowMouseMessages.fetch_add(1, std::memory_order_relaxed);
        if (keyRange) extraWindowKeyMessages.fetch_add(1, std::memory_order_relaxed);
        if (message == WM_MOUSEWHEEL) {
            extraVerticalWheel.fetch_add(GET_WHEEL_DELTA_WPARAM(wParam), std::memory_order_relaxed);
        } else if (message == WM_MOUSEHWHEEL) {
            extraHorizontalWheel.fetch_add(GET_WHEEL_DELTA_WPARAM(wParam), std::memory_order_relaxed);
        }
        if (mouseRange || keyRange || message == WM_INPUT || message == WM_CHAR) return 0;
    }
    WNDPROC original{};
    {
        std::scoped_lock lock(extraProceduresMutex);
        const auto found = extraOriginalProcedures.find(window);
        if (found != extraOriginalProcedures.end()) original = found->second;
    }
    if (original) return CallWindowProcW(original, window, message, wParam, lParam);
    return DefWindowProcW(window, message, wParam, lParam);
}

// Minecraft re-clips the cursor to a tiny region at the window center on its
// own simulation tick to measure mouse-look delta. If we only called
// ClipCursor(nullptr) once per render frame from our side, the game's next
// tick would immediately re-lock it, racing with us: the cursor would be
// unusable in the menu and the clip/unlock transition could hand the game a
// bogus delta (a camera "snap"). Hooking ClipCursor itself and refusing every
// call - from the game or anyone else - while the menu is open removes the
// race entirely, matching the fix used by Flarial's dll-oss ClipCursorHook.
BOOL WINAPI HookClipCursor(const RECT* rect) noexcept {
    if (menuVisible) return FALSE;
    return originalClipCursor ? originalClipCursor(rect) : ClipCursor(rect);
}

// Minecraft Bedrock is a CoreWindow (WinRT) app. Flarial's dll-oss tracks a
// SEPARATE cursor-visibility concept at the WinRT level
// (CoreWindow::PointerCursor) alongside the classic Win32 ShowCursor/
// ClipCursor calls - see WinrtUtils::setCursorTypeThreaded /
// installPointerHook. If Minecraft's own CoreWindow input routing checks
// THIS property (not the Win32 cursor state) to decide whether it is in
// mouselook mode, none of our Win32-only cursor handling so far would have
// any effect on it - which would explain every fix tried before this one
// having zero impact. CoreWindow::GetForCurrentThread() only succeeds when
// called from the thread that owns that CoreWindow, which may or may not be
// the thread Present() (and therefore this call) runs on - hence the
// diagnostic logging either way, so we know for certain rather than guess.
void SetCoreWindowCursorVisible(bool visible) noexcept {
    try {
        auto window = winrt::Windows::UI::Core::CoreWindow::GetForCurrentThread();
        if (!window) {
            static std::atomic_bool logged{};
            if (!logged.exchange(true)) {
                Diagnostics::LogLine("warning", "corewindow_cursor",
                    "CoreWindow::GetForCurrentThread() returned null - this thread does not own the CoreWindow");
            }
            return;
        }
        if (visible) {
            window.PointerCursor(winrt::Windows::UI::Core::CoreCursor(
                winrt::Windows::UI::Core::CoreCursorType::Arrow, 0));
        } else {
            window.PointerCursor(nullptr);
        }
        Diagnostics::LogLine("info", "corewindow_cursor",
            visible ? "Set CoreWindow PointerCursor to Arrow" : "Cleared CoreWindow PointerCursor");
    } catch (const winrt::hresult_error&) {
        Diagnostics::LogLine("warning", "corewindow_cursor", "CoreWindow access threw an hresult_error");
    } catch (...) {
        Diagnostics::LogLine("warning", "corewindow_cursor", "CoreWindow access threw an unknown exception");
    }
}

// Literal port of Flarial dll-oss's CursorHandler::releaseCursor (called
// when our menu opens and takes the mouse back from gameplay), same call
// order as theirs: unclip, release capture, then show the cursor.
void ReleaseCursorForMenu() noexcept {
    cursorHiddenBeforeMenu = !IsCursorVisible();
    SetCoreWindowCursorVisible(true);
    // Call the trampoline directly, not the (now hooked) ClipCursor symbol:
    // menuVisible is already true here, so going through the hook would make
    // this call a no-op too.
    if (originalClipCursor) originalClipCursor(nullptr); else ClipCursor(nullptr);
    ReleaseCapture();
    int count = ShowCursor(TRUE);
    showCursorCountBeforeMenu = count - 1;
    cursorStateSaved = true;
    while (count < 0) count = ShowCursor(TRUE);
    SetCursor(LoadCursorW(nullptr, IDC_ARROW));
}

// Returns the ShowCursor display count to what the game had before the menu
// opened. Bedrock hides the gameplay cursor by clearing its shape, not via
// ShowCursor, so forcing the count negative here (as this used to) left the
// pointer invisible on every game screen - inventory, chat, pause - until
// restart.
void RestoreShowCursorCount() noexcept {
    if (!cursorStateSaved) return;
    cursorStateSaved = false;
    int count = ShowCursor(FALSE);
    while (count > showCursorCountBeforeMenu) count = ShowCursor(FALSE);
    while (count < showCursorCountBeforeMenu) count = ShowCursor(TRUE);
}

LPARAM KeyboardMessageData(std::uint32_t key, bool released) noexcept {
    const auto scanCode = MapVirtualKeyW(key, MAPVK_VK_TO_VSC);
    std::uint64_t value = 1ULL | (static_cast<std::uint64_t>(scanCode) << 16U);
    if (released) value |= (1ULL << 30U) | (1ULL << 31U);
    return static_cast<LPARAM>(value);
}

// Private message used to hand synthetic key events to the game from a thread
// that isn't the window thread.
//
// These used to be dispatched with CallWindowProcW straight from HookPresent,
// i.e. running Minecraft's window procedure on the render thread concurrently
// with its real window thread - and ReleaseTrackedKeys can emit up to 256 of
// them in one go. Posting instead lets the game's own thread drain them in
// order. It has to be a private message rather than a plain posted WM_KEYUP
// because a posted key message would re-enter OUR subclass, which deliberately
// swallows key messages while the menu is open - so the release would never
// reach the game at all. WindowProcedure forwards this one to the original
// procedure directly.
constexpr UINT SyntheticKeyMessage = WM_APP + 0x1A9;

void PostSyntheticKey(std::uint32_t key, bool pressed) noexcept {
    if (!gameWindow) return;
    (void)PostMessageW(gameWindow, SyntheticKeyMessage, key, pressed ? 1 : 0);
}

void ReleaseTrackedKeys() noexcept {
    if (!gameWindow || !originalWindowProcedure) return;
    std::vector<std::uint32_t> released;
    {
        std::scoped_lock keyLock(keyStateMutex);
        for (std::uint32_t key = 0; key < 256; ++key) {
            if ((GetAsyncKeyState(static_cast<int>(key)) & 0x8000) != 0) keyStateTracker.Observe(key, true);
        }
        released = keyStateTracker.BeginIsolation();
    }
    for (const auto key : released) {
        PostSyntheticKey(key, false);
        if (moduleManager) moduleManager->Publish(KeyboardEvent{key, false, false});
    }
}

void RestoreTrackedKeys() noexcept {
    if (!gameWindow || !originalWindowProcedure) return;
    std::vector<std::uint32_t> restored;
    {
        std::scoped_lock keyLock(keyStateMutex);
        std::array<bool, 256> physical{};
        for (std::uint32_t key = 0; key < physical.size(); ++key) {
            physical[key] = (GetAsyncKeyState(static_cast<int>(key)) & 0x8000) != 0;
        }
        restored = keyStateTracker.EndIsolation(physical);
    }
    for (const auto key : restored) {
        PostSyntheticKey(key, true);
        if (moduleManager) moduleManager->Publish(KeyboardEvent{key, true, false});
    }
}

// Null Movement and friends: the first enabled IKeyTransformModule that
// takes the message replaces it with its own key messages, sent straight
// to the game's procedure on this (the window) thread.
bool TransformKeyMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam) noexcept {
    try {
        const auto modules = moduleManager->EnabledModulesOf<IKeyTransformModule>();
        if (modules.empty()) return false;
        std::array<bool, 256> physical{};
        for (std::uint32_t key = 1; key < physical.size(); ++key) {
            physical[key] = (GetAsyncKeyState(static_cast<int>(key)) & 0x8000) != 0;
        }
        const auto key = static_cast<std::uint32_t>(wParam);
        const bool pressed = message == WM_KEYDOWN;
        const bool repeated = pressed && (lParam & (1LL << 30)) != 0;
        std::vector<KeyMessage> out;
        for (auto* module : modules) {
            if (!module->TransformKey(key, pressed, repeated, physical, out)) continue;
            for (const auto& replacement : out) {
                LPARAM data = KeyboardMessageData(replacement.key, !replacement.pressed);
                if (replacement.pressed && replacement.key == key && repeated) data |= (1LL << 30);
                (void)CallWindowProcW(originalWindowProcedure, window, replacement.pressed ? WM_KEYDOWN : WM_KEYUP,
                    replacement.key, data);
            }
            return true;
        }
    } catch (...) {}
    return false;
}

// Per frame: mouse shaping, key-transform resets and held-key restores,
// keyed off whether gameplay currently owns the input.
void ApplyGameplayInput(bool gameFocused, bool cursorVisible) noexcept {
    if (!moduleManager) return;
    const bool captured = gameFocused && !cursorVisible && !menuVisible.load();
    MouseShaping shaping;
    try {
        for (const auto* module : moduleManager->EnabledModulesOf<IMouseShapingModule>()) module->ShapeMouse(shaping, captured);
    } catch (...) {
        shaping = {};
    }
    MouseInputBlocker::SetMouseShaping(shaping);
    if (captured == lastGameplayCaptured) return;
    lastGameplayCaptured = captured;
    gameplayCaptured.store(captured, std::memory_order_release);
    try {
        if (!captured) {
            for (auto* module : moduleManager->EnabledModulesOf<IKeyTransformModule>()) module->ResetKeyTransform();
            return;
        }
        // A game screen (inventory, chat, pause) just closed: Bedrock forgot
        // the keys still held through it.
        std::vector<std::uint32_t> keys;
        for (const auto* module : moduleManager->EnabledModulesOf<IHeldKeyRestoreModule>()) {
            for (const auto key : module->KeysToRestore()) keys.push_back(key);
        }
        std::ranges::sort(keys);
        keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
        for (const auto key : keys) {
            if (key != 0 && key < 256 && (GetAsyncKeyState(static_cast<int>(key)) & 0x8000) != 0) PostSyntheticKey(key, true);
        }
    } catch (...) {}
}

void PostProcessCallback(const ImDrawList*, const ImDrawCmd*) {
    const auto& io = ImGui::GetIO();
    postProcess.Bind(commandList.Get(), io.DisplaySize.x, io.DisplaySize.y, framePostProcess);
}

bool KeyDown(std::uint32_t key) noexcept {
    return key != 0 && key < 256 && (GetAsyncKeyState(static_cast<int>(key)) & 0x8000) != 0;
}

void ReleaseAssistKeys() noexcept {
    for (const auto key : heldAssistKeys) PostSyntheticKey(key, false);
    heldAssistKeys.clear();
}

// Assist keys use the same scoped window-message path as ordinary input.
// Sprint remains conditional on physical movement; generalized holds (such
// as toggle sneak) must reset themselves whenever gameplay loses the cursor.
void ApplyKeyHoldAssists(bool gameFocused, bool cursorVisible) noexcept {
    if (!gameWindow || !originalWindowProcedure || !moduleManager) { ReleaseAssistKeys(); return; }
    std::vector<std::uint32_t> wanted;
    try {
        for (auto* module : moduleManager->EnabledModulesOf<ISprintAssistModule>()) {
            const SprintInputs inputs{gameFocused && KeyDown(module->ForwardKey()), gameFocused && KeyDown(module->SneakKey()),
                !cursorVisible, menuVisible.load(), gameFocused};
            if (module->WantsSprintHeld(inputs) && module->SprintKey() != 0) wanted.push_back(module->SprintKey());
        }
        const HeldKeyInputs inputs{!cursorVisible, menuVisible.load(), gameFocused};
        for (auto* module : moduleManager->EnabledModulesOf<IKeyHoldAssistModule>()) {
            if (const auto key = module->DesiredHeldKey(inputs); key != 0) wanted.push_back(key);
        }
    } catch (...) {
        wanted.clear();
    }
    std::ranges::sort(wanted);
    wanted.erase(std::unique(wanted.begin(), wanted.end()), wanted.end());
    for (const auto key : heldAssistKeys) {
        if (!std::ranges::binary_search(wanted, key)) PostSyntheticKey(key, false);
    }
    for (const auto key : wanted) {
        if (!std::ranges::binary_search(heldAssistKeys, key)) PostSyntheticKey(key, true);
    }
    heldAssistKeys = std::move(wanted);
}

// Dynamic FPS: while the game is in the background, an enabled
// IFramePacingModule caps the presentation rate by waiting before handing
// the frame to the real Present(). The wait is bounded (MaxPacingDelay) and
// interruptible by Remove(), so unload stays prompt; it touches no
// swapchain or command-list state.
void ApplyFramePacing(bool gameFocused) noexcept {
    if (!moduleManager || !gameWindow) return;
    std::optional<std::chrono::microseconds> interval;
    try {
        const bool minimized = IsIconic(gameWindow) != FALSE;
        for (const auto* module : moduleManager->EnabledModulesOf<IFramePacingModule>()) {
            const auto candidate = module->FrameInterval(gameFocused, minimized);
            if (candidate && (!interval || *candidate > *interval)) interval = candidate;
        }
    } catch (...) {
        return;
    }
    if (!interval || lastPresentAt == std::chrono::steady_clock::time_point{}) return;
    const auto delay = Modules::PacingDelay(lastPresentAt, std::chrono::steady_clock::now(), *interval);
    const auto milliseconds = static_cast<DWORD>(std::chrono::ceil<std::chrono::milliseconds>(delay).count());
    if (milliseconds == 0) return;
    if (pacingWakeEvent) (void)WaitForSingleObject(pacingWakeEvent, milliseconds);
}

void ClearImGuiInput() noexcept {
    std::scoped_lock lock(imguiInputMutex);
    if (!imguiReady) return;
    auto& io = ImGui::GetIO();
    io.ClearInputKeys();
    io.ClearInputMouse();
    io.InputQueueCharacters.resize(0);
}

// Literal port of Flarial dll-oss's CursorHandler::grabCursor (called when
// gameplay regains the mouse, i.e. our menu closes). Deliberately does NOT
// call SetCursorPos - that warp was our own addition and not present in
// their code; it is a plausible source of the camera snap, since it would
// feed a large one-off position change into whatever reads raw cursor
// position (GameInput's underlying driver may sample from the same source
// SetCursorPos writes to, ahead of our GameInput-level freeze).
void RestoreCursorToGame() noexcept {
    if (!gameWindow) return;
    const bool regrab = cursorHiddenBeforeMenu;
    RestoreShowCursorCount();
    // Opened from a screen that shows the cursor (the pause screen): hand it
    // back visible and free, exactly as that screen had it.
    if (!regrab) return;
    RECT rect{};
    GetClientRect(gameWindow, &rect);
    rect.top = (rect.bottom + rect.top) / 2;
    rect.left = (rect.right + rect.left) / 2;
    ClientToScreen(gameWindow, reinterpret_cast<POINT*>(&rect));
    rect.bottom = rect.top;
    rect.right = rect.left;
    ClipCursor(&rect);
    SetCapture(gameWindow);
    SetCoreWindowCursorVisible(false);
}

// Closing the menu because the game lost focus is NOT the same as the player
// closing it: the cursor must not be clipped or captured to a window that
// another app has just taken focus from (Minecraft re-grabs it itself on focus
// gain). So this unwinds exactly the state ReleaseCursorForMenu set - the
// ShowCursor refcount and the CoreWindow cursor - and nothing more.
//
// Assigning menuVisible directly here, as this path used to, skipped all of
// it: after alt-tabbing away with the menu open and returning, the cursor
// stayed visible and unclipped during gameplay.
void CloseMenuForFocusLoss() noexcept {
    MouseInputBlocker::SetActive(false);
    const bool wasVisible = menuVisible.exchange(false, std::memory_order_acq_rel);
    const bool wasTheater = menuIsReplayTheater.exchange(false, std::memory_order_acq_rel);
    menuIsReplayEditor.store(false, std::memory_order_release);
    if (wasVisible) {
        PlayerSkinTelemetry::NotifyMenuClosed();
        ClearImGuiInput();
        // This runs on the window thread; ShowCursor counts are per thread,
        // so the render thread unwinds its own count on the next Present.
        if (!wasTheater) cursorRestorePending.store(true, std::memory_order_release);
    }
    // Drop isolation bookkeeping without synthesizing key events into a window
    // that no longer has focus. Guarded because the render thread also touches
    // the tracker (ReleaseTrackedKeys/RestoreTrackedKeys).
    std::scoped_lock keyLock(keyStateMutex);
    keyStateTracker.Reset();
}

enum class MenuKind { ClickGui, ReplayTheater, ReplayEditor };

void SetMenuVisible(bool visible, MenuKind kind = MenuKind::ClickGui) noexcept {
    if (menuVisible == visible) return;
    if (visible) {
        menuIsReplayTheater.store(kind == MenuKind::ReplayTheater, std::memory_order_release);
        menuIsReplayEditor.store(kind == MenuKind::ReplayEditor, std::memory_order_release);
    }
    menuVisible = visible;
    MouseInputBlocker::SetActive(menuVisible);
    if (menuIsReplayTheater.load(std::memory_order_acquire)) {
        if (menuVisible) {
            ReleaseTrackedKeys();
        } else {
            ClearImGuiInput();
            RestoreTrackedKeys();
            menuIsReplayTheater.store(false, std::memory_order_release);
        }
        return;
    }
    if (menuVisible) {
        // The event-driven skin hook and startup cache keep the menu portrait
        // current. A refresh here used to start a large game-memory scan on
        // the same moment as the first ClickGUI frame, causing a visible hitch.
        ReleaseTrackedKeys();
        ReleaseCursorForMenu();
    } else {
        PlayerSkinTelemetry::NotifyMenuClosed();
        ClearImGuiInput();
        RestoreCursorToGame();
        RestoreTrackedKeys();
        menuIsReplayEditor.store(false, std::memory_order_release);
    }
}

void UpdateImGuiMouseFromSystem() noexcept {
    if ((!menuVisible && !overlayControlsVisible.load(std::memory_order_acquire)) || !gameWindow || !imguiReady) return;

    POINT cursor{};
    if (GetCursorPos(&cursor) && ScreenToClient(gameWindow, &cursor)) {
        ImGui::GetIO().AddMousePosEvent(static_cast<float>(cursor.x), static_cast<float>(cursor.y));
    }

    ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left,
        (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0);
    ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Right,
        (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0);
    ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Middle,
        (GetAsyncKeyState(VK_MBUTTON) & 0x8000) != 0);

    const float vertical = static_cast<float>(extraVerticalWheel.exchange(0, std::memory_order_acq_rel))
        / static_cast<float>(WHEEL_DELTA);
    const float horizontal = static_cast<float>(extraHorizontalWheel.exchange(0, std::memory_order_acq_rel))
        / static_cast<float>(WHEEL_DELTA);
    if (vertical != 0.0F || horizontal != 0.0F) ImGui::GetIO().AddMouseWheelEvent(horizontal, vertical);
}

class CallbackGuard final {
public:
    CallbackGuard() noexcept { activeCallbacks.fetch_add(1, std::memory_order_acq_rel); }
    ~CallbackGuard() { activeCallbacks.fetch_sub(1, std::memory_order_acq_rel); }
    CallbackGuard(const CallbackGuard&) = delete;
    CallbackGuard& operator=(const CallbackGuard&) = delete;
};

void NeutralizeRawMouse(RAWINPUT& input) noexcept {
    if (input.header.dwType != RIM_TYPEMOUSE || !menuVisible) return;
    rawMousePacketsBlocked.fetch_add(1, std::memory_order_relaxed);
    input.data.mouse.usFlags = 0;
    input.data.mouse.usButtonFlags = 0;
    input.data.mouse.usButtonData = 0;
    input.data.mouse.ulRawButtons = 0;
    input.data.mouse.lLastX = 0;
    input.data.mouse.lLastY = 0;
}

UINT WINAPI HookGetRawInputData(HRAWINPUT input, UINT command, LPVOID data, PUINT size, UINT headerSize) noexcept {
    const CallbackGuard callback;
    if (!originalGetRawInputData) return static_cast<UINT>(-1);
    const UINT result = originalGetRawInputData(input, command, data, size, headerSize);
    if (command == RID_INPUT && data && result != static_cast<UINT>(-1)
        && result >= sizeof(RAWINPUTHEADER) + sizeof(RAWMOUSE)) {
        NeutralizeRawMouse(*static_cast<RAWINPUT*>(data));
    }
    return result;
}

UINT WINAPI HookGetRawInputBuffer(PRAWINPUT data, PUINT size, UINT headerSize) noexcept {
    const CallbackGuard callback;
    if (!originalGetRawInputBuffer) return static_cast<UINT>(-1);
    // Capture the caller's buffer size BEFORE the call so the walk below can
    // be bounded: dwSize comes from the buffer we are iterating, so trusting
    // it blindly let a bogus value walk the cursor past the end of the
    // caller's buffer - and NeutralizeRawMouse writes through that cursor.
    const UINT bufferBytes = size ? *size : 0;
    const UINT count = originalGetRawInputBuffer(data, size, headerSize);
    if (!data || count == static_cast<UINT>(-1)) return count;
    const auto start = reinterpret_cast<std::uintptr_t>(data);
    const auto end = start + bufferBytes;
    auto* current = data;
    for (UINT index = 0; index < count; ++index) {
        const auto address = reinterpret_cast<std::uintptr_t>(current);
        if (address < start || address > end || end - address < sizeof(RAWINPUT)) break;
        NeutralizeRawMouse(*current);
        const auto advance = current->header.dwSize;
        if (advance < sizeof(RAWINPUTHEADER) || advance > end - address) break;
        const auto next = (address + advance + sizeof(std::uintptr_t) - 1U) & ~(sizeof(std::uintptr_t) - 1U);
        if (next <= address) break;
        current = reinterpret_cast<PRAWINPUT>(next);
    }
    return count;
}

// Waits (bounded) for every command list we submitted to finish before our
// resources are released - releasing a texture the GPU is still reading
// from is undefined behavior and a classic device-removed trigger.
void WaitForOverlayGpuIdle() noexcept {
    auto* queue = commandQueue.load(std::memory_order_acquire);
    if (!queue || !renderFence || !fenceEvent) return;
    const UINT64 value = nextFenceValue++;
    if (FAILED(queue->Signal(renderFence.Get(), value))) return;
    if (renderFence->GetCompletedValue() >= value) return;
    if (SUCCEEDED(renderFence->SetEventOnCompletion(value, fenceEvent))) (void)WaitForSingleObject(fenceEvent, 500);
}

void ReleaseRenderState() noexcept {
    WaitForOverlayGpuIdle();
    frameBlend.Release();
    postProcess.Release();
    videoCapture.Release();
    std::scoped_lock imguiLock(imguiInputMutex);
    if (imguiReady) {
        ImGui_ImplDX12_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        imguiReady = false;
    }
    // SetWindowLongPtrW(GWLP_WNDPROC) fails when the window belongs to another
    // thread. If un-subclassing fails our WindowProcedure stays installed, so
    // the module must not be unloaded afterwards - the next window message
    // would jump into unmapped memory.
    if (gameWindow && originalWindowProcedure) {
        SetLastError(0);
        if (SetWindowLongPtrW(gameWindow, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(originalWindowProcedure)) == 0
            && GetLastError() != 0) {
            Diagnostics::LogLine("warning", "unsubclass_failed", std::format(
                "could not restore the game window procedure (Win32 {})", GetLastError()));
            UnloadGuard::Block("the game window procedure could not be restored");
        }
    }
    originalWindowProcedure = nullptr;
    gameWindow = nullptr;
    {
        std::scoped_lock extraLock(extraProceduresMutex);
        for (const auto& [window, original] : extraOriginalProcedures) {
            if (!original) continue;
            SetLastError(0);
            if (SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(original)) == 0
                && GetLastError() != 0) {
                Diagnostics::LogLine("warning", "unsubclass_failed", std::format(
                    "could not restore an auxiliary window procedure (Win32 {})", GetLastError()));
                UnloadGuard::Block("an auxiliary window procedure could not be restored");
            }
        }
        extraOriginalProcedures.clear();
    }
    frames.clear();
    renderFence.Reset();
    if (fenceEvent) CloseHandle(fenceEvent);
    fenceEvent = nullptr;
    nextFenceValue = 1;
    srvHeap.Reset();
    rtvHeap.Reset();
    commandList.Reset();
    device.Reset();
}

LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    // Synthetic key events posted from the render thread. Forwarded straight
    // to the game's own procedure so they bypass the menu input-swallowing
    // below (the whole point is to tell the game a key was released *because*
    // the menu just opened). Handled before anything else so it is never
    // counted as real input or fed back into keyStateTracker.
    if (message == SyntheticKeyMessage) {
        if (!originalWindowProcedure) return 0;
        const auto key = static_cast<std::uint32_t>(wParam);
        const bool pressed = lParam != 0;
        return CallWindowProcW(originalWindowProcedure, window, pressed ? WM_KEYDOWN : WM_KEYUP,
            key, KeyboardMessageData(key, !pressed));
    }

    bool allowGameplay = true;
    if (menuVisible) {
        const bool mouseRange = message >= WM_MOUSEFIRST && message <= WM_MOUSELAST;
        const bool keyRange = message >= WM_KEYFIRST && message <= WM_KEYLAST;
        if (mouseRange) mouseMessagesWhileMenu.fetch_add(1, std::memory_order_relaxed);
        if (keyRange) keyboardMessagesWhileMenu.fetch_add(1, std::memory_order_relaxed);
        if (message == WM_INPUT) rawInputMessagesWhileMenu.fetch_add(1, std::memory_order_relaxed);
    }
    const bool keyboardMessage = message == WM_KEYDOWN || message == WM_KEYUP
        || message == WM_SYSKEYDOWN || message == WM_SYSKEYUP;
    if (keyboardMessage) {
        const bool pressed = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
        const bool repeated = (lParam & (1LL << 30)) != 0;
        if (pressed && !repeated && wParam == VK_ESCAPE && !menuVisible) {
            pauseProbeRequested.store(true, std::memory_order_release);
        }
        {
            std::scoped_lock keyLock(keyStateMutex);
            keyStateTracker.Observe(static_cast<std::uint32_t>(wParam), pressed);
        }
        if (moduleManager) {
            // A game screen with a cursor (chat above all) owns typing as much
            // as the ClickGUI does: letters typed there are not gameplay
            // presses, so module keys and keybinds must ignore them.
            const bool gameScreenOpen = !gameplayCaptured.load(std::memory_order_acquire);
            const KeyboardEvent event{static_cast<std::uint32_t>(wParam), pressed,
                repeated, menuVisible || overlayControlsVisible.load(std::memory_order_acquire) || gameScreenOpen};
            allowGameplay = moduleManager->PublishInput(event);
            if (pressed && !event.repeated && !event.uiCaptured && wParam != configuredMenuKey.load()
                && wParam != configuredUnloadKey.load()) {
                // Only mark the settings dirty when a keybind actually toggled
                // something. Setting it unconditionally meant every gameplay
                // keypress triggered a full delete-and-recreate of
                // settings.json (SettingsStore::Save), at up to 20 Hz.
                if (moduleManager->HandleKeybind(static_cast<std::uint32_t>(wParam))) {
                    settingsDirty.store(true);
                }
            }
        }
    }

    const bool mouseMessage = message >= WM_MOUSEFIRST && message <= WM_MOUSELAST;
    if (mouseMessage && moduleManager) {
        POINT position{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (message == WM_MOUSEWHEEL || message == WM_MOUSEHWHEEL) ScreenToClient(window, &position);
        std::uint32_t buttons{};
        if ((wParam & MK_LBUTTON) != 0) buttons |= 1U;
        if ((wParam & MK_RBUTTON) != 0) buttons |= 2U;
        if ((wParam & MK_MBUTTON) != 0) buttons |= 4U;
        if ((wParam & MK_XBUTTON1) != 0) buttons |= 8U;
        if ((wParam & MK_XBUTTON2) != 0) buttons |= 16U;
        std::uint32_t changedButton{};
        bool pressed{};
        switch (message) {
        case WM_LBUTTONDOWN: changedButton = 1U; pressed = true; break;
        case WM_LBUTTONUP: changedButton = 1U; break;
        case WM_RBUTTONDOWN: changedButton = 2U; pressed = true; break;
        case WM_RBUTTONUP: changedButton = 2U; break;
        case WM_MBUTTONDOWN: changedButton = 4U; pressed = true; break;
        case WM_MBUTTONUP: changedButton = 4U; break;
        case WM_XBUTTONDOWN: changedButton = GET_XBUTTON_WPARAM(wParam) == XBUTTON1 ? 8U : 16U; pressed = true; break;
        case WM_XBUTTONUP: changedButton = GET_XBUTTON_WPARAM(wParam) == XBUTTON1 ? 8U : 16U; break;
        default: break;
        }
        const float wheel = message == WM_MOUSEWHEEL || message == WM_MOUSEHWHEEL
            ? static_cast<float>(GET_WHEEL_DELTA_WPARAM(wParam)) / static_cast<float>(WHEEL_DELTA) : 0.0F;
        // The validated MouseDevice::feed hook is authoritative for button
        // transitions. Publishing the legacy message too would make one
        // physical click advance debounce/CPS state twice. Keep the window
        // path as a fallback, and continue publishing movement/wheel here.
        if (changedButton == 0U || !MouseInputBlocker::CanFilterButtons()) {
            const MouseEvent event{static_cast<float>(position.x), static_cast<float>(position.y), wheel, buttons,
                changedButton, pressed, menuVisible || overlayControlsVisible.load(std::memory_order_acquire)};
            allowGameplay = moduleManager->PublishInput(event) && allowGameplay;
        }
    }
    // Replay hotbar without the mouse feed hook: take the wheel here.
    if ((message == WM_MOUSEWHEEL) && replayHotbarActive.load(std::memory_order_acquire)
        && !MouseInputBlocker::ReplayHotbarCaptured()) {
        const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
        if (delta != 0) replayHotbarScroll.fetch_add(delta > 0 ? 1 : -1, std::memory_order_acq_rel);
        return 0;
    }
    if (message == WM_KILLFOCUS) {
        CloseMenuForFocusLoss();
        hudEditMode = false;
    }
    const bool controlsVisible = overlayControlsVisible.load(std::memory_order_acquire);
    if (menuVisible || controlsVisible) {
        bool wantCaptureMouse = false;
        {
            std::scoped_lock imguiLock(imguiInputMutex);
            if (imguiReady) {
                (void)ImGui_ImplWin32_WndProcHandler(window, message, wParam, lParam);
                wantCaptureMouse = ImGui::GetIO().WantCaptureMouse;
            }
        }
        const bool keyMessage = message >= WM_KEYFIRST && message <= WM_KEYLAST;
        if (menuVisible && (mouseMessage || keyMessage || message == WM_INPUT || message == WM_CHAR)) return 0;
        if (controlsVisible && mouseMessage && wantCaptureMouse) return 0;
    }
    // The menu's own toggle key is read via GetAsyncKeyState polling in the
    // Present hook, on a completely separate timeline from this Win32 message
    // pump. menuVisible only flips *after* that poll runs, so the raw
    // WM_KEYDOWN/WM_KEYUP for the very keypress that opens the menu always
    // arrives here first and would otherwise fall through to the game, every
    // time, regardless of menuVisible. 'K' is reserved for us and must never
    // reach gameplay, independent of menu state.
    if (keyboardMessage && (wParam == configuredMenuKey.load() || wParam == configuredUnloadKey.load())) return 0;
    if (!allowGameplay) return 0;
    if ((message == WM_KEYDOWN || message == WM_KEYUP) && moduleManager && !menuVisible
        && gameplayCaptured.load(std::memory_order_acquire) && TransformKeyMessage(window, message, wParam, lParam)) {
        return 0;
    }
    return CallWindowProcW(originalWindowProcedure, window, message, wParam, lParam);
}

// Ported from Flarial dll-oss's WindowManager::findGameWindow, matching its
// exact priority order (not just the GDK fallback half of it, which is what
// an earlier pass here mistakenly used as the *only* method). Minecraft's
// real input-receiving window is not reliably the HWND the DXGI swap chain
// reports via GetDesc().OutputWindow for this title - confirmed by the
// scroll wheel changing the hotbar instead of scrolling our menu while only
// the swap chain's window was subclassed.
HWND FindBedrockWindow() noexcept {
    const DWORD currentProcessId = GetCurrentProcessId();

    // Primary path (what Flarial uses for the Microsoft Store/UWP package,
    // which is what this project targets): match by window TITLE "Minecraft",
    // not class. Verify process ownership ourselves for safety - their code
    // doesn't, but there's no reason not to check here.
    if (HWND byTitle = FindWindowW(nullptr, L"Minecraft")) {
        DWORD ownerProcessId{};
        GetWindowThreadProcessId(byTitle, &ownerProcessId);
        if (ownerProcessId == currentProcessId) return byTitle;
    }

    // GDK-build fallback: match by class "Bedrock", filtered to our process.
    HWND window{};
    while ((window = FindWindowExW(nullptr, window, L"Bedrock", nullptr)) != nullptr) {
        DWORD ownerProcessId{};
        GetWindowThreadProcessId(window, &ownerProcessId);
        if (ownerProcessId == currentProcessId) return window;
    }
    return nullptr;
}

std::string NarrowAscii(std::wstring_view value) {
    std::string result; result.reserve(value.size());
    for (const wchar_t ch : value) result.push_back(ch <= 0x7f ? static_cast<char>(ch) : '?');
    return result;
}

// UWP titles like Minecraft can present through a window that is not the same
// HWND actually receiving raw mouse/keyboard input (a separate CoreWindow).
// Logs the swap chain's OutputWindow alongside every other top-level window
// owned by this process, and subclasses every one of them (other than the
// one already wired to ImGui) purely to swallow mouse/keyboard/WM_INPUT while
// the menu is open - see ExtraInputBlockingProcedure.
void LogAndSecureWindowTopology(HWND captured) noexcept {
    // Deliberately avoid GetWindowTextW here: it sends a synchronous WM_GETTEXT
    // to the target window's own queue, and calling that mid-Present (before our
    // subclass hook is installed, and possibly cross-thread for other windows
    // this process owns) risks a deadlock the OS then kills as a hang. Class
    // names come from the window class table and never dispatch a message, so
    // they are safe to query here.
    wchar_t className[256]{};
    GetClassNameW(captured, className, static_cast<int>(std::size(className)));
    Diagnostics::LogLine("info", "window_topology", std::format(
        "swapchain OutputWindow hwnd={:#x} class={}",
        reinterpret_cast<std::uintptr_t>(captured), NarrowAscii(className)));

    struct EnumState { DWORD processId; HWND captured; } state{GetCurrentProcessId(), captured};
    EnumWindows([](HWND window, LPARAM param) -> BOOL {
        const auto& enumState = *reinterpret_cast<EnumState*>(param);
        DWORD ownerProcessId{};
        GetWindowThreadProcessId(window, &ownerProcessId);
        if (ownerProcessId != enumState.processId) return TRUE;
        wchar_t windowClassName[256]{};
        GetClassNameW(window, windowClassName, static_cast<int>(std::size(windowClassName)));
        Diagnostics::LogLine("info", "window_topology", std::format(
            "process window hwnd={:#x} class={} visible={}",
            reinterpret_cast<std::uintptr_t>(window), NarrowAscii(windowClassName),
            IsWindowVisible(window) ? "true" : "false"));
        if (window == enumState.captured) return TRUE;
        const auto original = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(window, GWLP_WNDPROC,
            reinterpret_cast<LONG_PTR>(ExtraInputBlockingProcedure)));
        if (original) {
            std::scoped_lock lock(extraProceduresMutex);
            extraOriginalProcedures[window] = original;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&state));
}

std::vector<std::uint8_t> uiFontBytes;

// Load the shipped Oinkster face at deliberate UI roles. ImGui packs every
// size into one atlas/SRV, so this improves hierarchy without adding renderer
// resources or a second font dependency.
bool LoadEmbeddedUiFonts(ImGuiIO& io) {
    HMODULE module{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&LoadEmbeddedUiFonts), &module)) return false;
    const HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(IDR_OINKSTER_UI_FONT), RT_RCDATA);
    if (!resource) return false;
    const HGLOBAL loaded = LoadResource(module, resource);
    auto* data = static_cast<std::uint8_t*>(LockResource(loaded));
    const DWORD dataSize = SizeofResource(module, resource);
    if (!data || dataSize == 0) return false;
    uiFontBytes.assign(data, data + dataSize);

    ImFontConfig config{};
    config.FontDataOwnedByAtlas = false;
    ImFont* compact = io.Fonts->AddFontFromMemoryTTF(uiFontBytes.data(), static_cast<int>(uiFontBytes.size()), 14.0F, &config);
    ImFont* body = io.Fonts->AddFontFromMemoryTTF(uiFontBytes.data(), static_cast<int>(uiFontBytes.size()), 18.0F, &config);
    ImFont* title = io.Fonts->AddFontFromMemoryTTF(uiFontBytes.data(), static_cast<int>(uiFontBytes.size()), 24.0F, &config);
    if (!compact || !body || !title) return false;
    io.FontDefault = body;
    OverlayUi::ConfigureFonts(compact, body, title);
    return true;
}

bool InitializeRenderer(IDXGISwapChain3* swapChain) {
    if (!commandQueue.load(std::memory_order_acquire) || FAILED(swapChain->GetDevice(IID_PPV_ARGS(&device)))) return false;
    DXGI_SWAP_CHAIN_DESC description{};
    if (FAILED(swapChain->GetDesc(&description)) || description.BufferCount == 0) return false;
    const HWND bedrockWindow = FindBedrockWindow();
    gameWindow = bedrockWindow ? bedrockWindow : description.OutputWindow;
    Diagnostics::LogLine("info", "window_topology", std::format(
        "swapchain OutputWindow hwnd={:#x} vs \"Bedrock\"-class window hwnd={:#x} (using {})",
        reinterpret_cast<std::uintptr_t>(description.OutputWindow), reinterpret_cast<std::uintptr_t>(bedrockWindow),
        bedrockWindow ? "Bedrock-class window" : "swapchain OutputWindow (fallback: none found)"));
    LogAndSecureWindowTopology(gameWindow);
    renderFormat = description.BufferDesc.Format;
    frames.resize(description.BufferCount);

    D3D12_DESCRIPTOR_HEAP_DESC rtvDescription{};
    rtvDescription.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvDescription.NumDescriptors = description.BufferCount;
    if (FAILED(device->CreateDescriptorHeap(&rtvDescription, IID_PPV_ARGS(&rtvHeap)))) return false;
    D3D12_DESCRIPTOR_HEAP_DESC srvDescription{};
    srvDescription.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvDescription.NumDescriptors = SrvDescriptorCount;
    srvDescription.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device->CreateDescriptorHeap(&srvDescription, IID_PPV_ARGS(&srvHeap)))) return false;
    if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&renderFence)))) return false;
    fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!fenceEvent) return false;

    const UINT increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    auto handle = rtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT index = 0; index < description.BufferCount; ++index) {
        if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&frames[index].allocator)))) return false;
        if (FAILED(swapChain->GetBuffer(index, IID_PPV_ARGS(&frames[index].backBuffer)))) return false;
        frames[index].rtv = handle;
        device->CreateRenderTargetView(frames[index].backBuffer.Get(), nullptr, handle);
        handle.ptr += increment;
    }
    if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, frames.front().allocator.Get(), nullptr, IID_PPV_ARGS(&commandList)))) return false;
    commandList->Close();

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().MouseDrawCursor = true;
    if (!LoadEmbeddedUiFonts(ImGui::GetIO())) {
        ImFont* fallback = ImGui::GetIO().Fonts->AddFontDefault();
        ImGui::GetIO().FontDefault = fallback;
        OverlayUi::ConfigureFonts(fallback, fallback, fallback);
    }
    ImGui::StyleColorsDark();
    auto& style = ImGui::GetStyle();
    style.WindowRounding = 10.0F;
    style.ChildRounding = 8.0F;
    style.FrameRounding = 6.0F;
    style.PopupRounding = 8.0F;
    style.GrabRounding = 6.0F;
    style.ScrollbarRounding = 6.0F;
    style.WindowBorderSize = 1.0F;
    style.ChildBorderSize = 1.0F;
    style.PopupBorderSize = 1.0F;
    style.FrameBorderSize = 0.0F;
    style.ScrollbarSize = 10.0F;
    // Palette mirrors OverlayUi.cpp's Canvas/Surface/Pink constants so every
    // stock ImGui widget (search box, sliders, combos, scrollbars) matches the
    // custom-drawn menu instead of falling back to ImGui's default blue.
    constexpr ImVec4 Canvas(0.094F, 0.071F, 0.086F, 0.98F);
    constexpr ImVec4 Surface(0.149F, 0.110F, 0.133F, 1.0F);
    constexpr ImVec4 SurfaceRaised(0.196F, 0.141F, 0.173F, 1.0F);
    constexpr ImVec4 Stroke(0.267F, 0.180F, 0.224F, 1.0F);
    constexpr ImVec4 Text(0.925F, 0.925F, 0.941F, 1.0F);
    constexpr ImVec4 Muted(0.627F, 0.541F, 0.588F, 1.0F);
    constexpr ImVec4 Pink(0.941F, 0.541F, 0.690F, 1.0F);
    constexpr ImVec4 PinkDeep(0.290F, 0.157F, 0.216F, 1.0F);
    constexpr ImVec4 Clear(0.0F, 0.0F, 0.0F, 0.0F);
    auto& colors = style.Colors;
    colors[ImGuiCol_Text] = Text;
    colors[ImGuiCol_TextDisabled] = Muted;
    colors[ImGuiCol_WindowBg] = Canvas;
    colors[ImGuiCol_ChildBg] = Clear;
    colors[ImGuiCol_PopupBg] = Surface;
    colors[ImGuiCol_Border] = Stroke;
    colors[ImGuiCol_BorderShadow] = Clear;
    colors[ImGuiCol_FrameBg] = SurfaceRaised;
    colors[ImGuiCol_FrameBgHovered] = Stroke;
    colors[ImGuiCol_FrameBgActive] = Stroke;
    colors[ImGuiCol_Separator] = Stroke;
    colors[ImGuiCol_SeparatorHovered] = Stroke;
    colors[ImGuiCol_SeparatorActive] = Pink;
    colors[ImGuiCol_ScrollbarBg] = Clear;
    colors[ImGuiCol_ScrollbarGrab] = Stroke;
    colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.329F, 0.227F, 0.275F, 1.0F);
    colors[ImGuiCol_ScrollbarGrabActive] = Muted;
    colors[ImGuiCol_CheckMark] = Pink;
    colors[ImGuiCol_SliderGrab] = Pink;
    colors[ImGuiCol_SliderGrabActive] = Pink;
    colors[ImGuiCol_Button] = SurfaceRaised;
    colors[ImGuiCol_ButtonHovered] = Stroke;
    colors[ImGuiCol_ButtonActive] = PinkDeep;
    colors[ImGuiCol_Header] = SurfaceRaised;
    colors[ImGuiCol_HeaderHovered] = Stroke;
    colors[ImGuiCol_HeaderActive] = PinkDeep;
    colors[ImGuiCol_TextSelectedBg] = ImVec4(Pink.x, Pink.y, Pink.z, 0.30F);
    colors[ImGuiCol_NavHighlight] = Pink;
    colors[ImGuiCol_ResizeGrip] = Clear;
    colors[ImGuiCol_ResizeGripHovered] = Stroke;
    colors[ImGuiCol_ResizeGripActive] = Pink;
    if (!ImGui_ImplWin32_Init(gameWindow)
        || !ImGui_ImplDX12_Init(device.Get(), description.BufferCount, renderFormat, srvHeap.Get(),
            srvHeap->GetCPUDescriptorHandleForHeapStart(), srvHeap->GetGPUDescriptorHandleForHeapStart())) return false;
    originalWindowProcedure = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(gameWindow, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(WindowProcedure)));
    imguiReady = originalWindowProcedure != nullptr;
    return imguiReady;
}

bool IsCursorVisible() noexcept {
    CURSORINFO info{ sizeof(CURSORINFO) };
    // SetCursor(nullptr) hides the pointer without clearing CURSOR_SHOWING,
    // so a missing cursor shape also counts as hidden.
    return GetCursorInfo(&info) && (info.flags & CURSOR_SHOWING) != 0 && info.hCursor != nullptr;
}

void LogPauseDetect(std::string_view what) noexcept {
    CURSORINFO info{ sizeof(CURSORINFO) };
    const bool read = GetCursorInfo(&info) != FALSE;
    Diagnostics::LogLine("info", "replay_pause_detect", std::format(
        "{} pauseVisible={} cursorRead={} flags=0x{:x} hasShape={} prevFrameCursorVisible={}",
        what, minecraftPauseVisible, read, read ? info.flags : 0, read && info.hCursor != nullptr,
        previousFrameCursorVisible));
}

// Minecraft's pause screen has no direct signal we can read, so it is
// inferred: Esc pressed during gameplay (cursor hidden on the previous
// frame), then the cursor appears within a short window. The latch then
// holds until the cursor stays hidden again, focus is lost, or the ClickGUI
// opens. Esc that closes a screen (cursor already visible) never arms it.
void UpdateMinecraftPauseVisible(bool cursorVisible, bool gameFocused,
    std::chrono::steady_clock::time_point now) noexcept {
    constexpr auto CandidateWindow = std::chrono::milliseconds(1500);
    constexpr auto HiddenRelease = std::chrono::milliseconds(150);
    const bool wasVisible = minecraftPauseVisible;
    if (pauseProbeRequested.exchange(false, std::memory_order_acq_rel)) {
        const bool armed = gameFocused && !menuVisible && !previousFrameCursorVisible;
        if (armed) pauseCandidateUntil = now + CandidateWindow;
        LogPauseDetect(armed ? "escape armed" : "escape ignored");
    }
    if (!gameFocused || menuVisible) {
        minecraftPauseVisible = false;
        pauseCandidateUntil = {};
    } else if (!minecraftPauseVisible) {
        const bool levelRendering = !WorldTelemetry::Attached()
            || WorldTelemetry::LevelRenderedWithin(now, std::chrono::milliseconds(250));
        if (cursorVisible && now <= pauseCandidateUntil && levelRendering) {
            minecraftPauseVisible = true;
            pauseCandidateUntil = {};
            pauseCursorHiddenSince = {};
        }
    } else if (WorldTelemetry::Attached()
        && !WorldTelemetry::LevelRenderedWithin(now, std::chrono::milliseconds(250))) {
        // Quitting from the pause screen leaves the cursor visible on the
        // main menu; the level no longer rendering is what ends the pause.
        minecraftPauseVisible = false;
        pauseCandidateUntil = {};
    } else if (cursorVisible) {
        pauseCursorHiddenSince = {};
    } else if (pauseCursorHiddenSince == std::chrono::steady_clock::time_point{}) {
        pauseCursorHiddenSince = now;
    } else if (now - pauseCursorHiddenSince >= HiddenRelease) {
        minecraftPauseVisible = false;
    }
    previousFrameCursorVisible = cursorVisible;
    if (wasVisible != minecraftPauseVisible) LogPauseDetect(minecraftPauseVisible ? "latched" : "released");
}

bool GameplayHudVisible(bool cursorVisible, bool gameFocused,
    std::chrono::steady_clock::time_point now) noexcept {
    constexpr auto Debounce = std::chrono::milliseconds(140);
    if (!cursorVisibilityInitialized) {
        cursorVisibilityInitialized = true;
        observedCursorVisible = cursorVisible;
        stableCursorVisible = cursorVisible;
        cursorVisibilityChangedAt = now;
    } else if (cursorVisible != observedCursorVisible) {
        observedCursorVisible = cursorVisible;
        cursorVisibilityChangedAt = now;
    } else if (cursorVisible != stableCursorVisible && now - cursorVisibilityChangedAt >= Debounce) {
        stableCursorVisible = cursorVisible;
    }
    return gameFocused && !stableCursorVisible;
}


void STDMETHODCALLTYPE HookExecute(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists) {
    const CallbackGuard callback;
    if (queue) {
        D3D12_COMMAND_QUEUE_DESC description = queue->GetDesc();
        if (description.Type == D3D12_COMMAND_LIST_TYPE_DIRECT && commandQueue.load(std::memory_order_relaxed) == nullptr) {
            queue->AddRef();
            ID3D12CommandQueue* expected{};
            if (!commandQueue.compare_exchange_strong(expected, queue, std::memory_order_release, std::memory_order_relaxed)) {
                queue->Release();
            }
        }
    }
    originalExecute(queue, count, lists);
}

HRESULT STDMETHODCALLTYPE HookPresent(IDXGISwapChain* baseSwapChain, UINT interval, UINT flags) {
    const CallbackGuard callback;
    // GetAsyncKeyState is process-wide, not per-window: without this gate,
    // typing the menu key in a browser or Discord toggled the overlay, and the
    // unload key ejected the client outright.
    const bool gameFocused = gameWindow && GetForegroundWindow() == gameWindow;
    const bool unloadDown = gameFocused
        && (GetAsyncKeyState(static_cast<int>(configuredUnloadKey.load())) & 0x8000) != 0;
    if (unloadDown && !unloadKeyDown) ejectRequested.store(true);
    unloadKeyDown = unloadDown;
    if (cursorRestorePending.exchange(false, std::memory_order_acq_rel) && !menuVisible) RestoreShowCursorCount();
    const bool cursorVisible = IsCursorVisible();
    const bool escapeDown = gameFocused && (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
    if (escapeDown && !escapeKeyDown && !menuVisible) {
        pauseProbeRequested.store(true, std::memory_order_release);
    }
    escapeKeyDown = escapeDown;
    // Outside a world (main menu, world list, settings) K does nothing; the
    // main-menu Replays button is the way in there. Without world telemetry
    // there is no way to tell, so K keeps working everywhere.
    const auto inWorldCheckedAt = std::chrono::steady_clock::now();
    // The level keeps rendering behind Minecraft's pause screen, so this
    // stays true while paused and turns false as soon as the world is left.
    const bool worldKnown = WorldTelemetry::Attached();
    const bool levelRendered = worldKnown
        && WorldTelemetry::LevelRenderedWithin(inWorldCheckedAt, std::chrono::milliseconds(400));
    const bool inWorld = !worldKnown || levelRendered;
    if (inWorld != loggedInWorld) {
        loggedInWorld = inWorld;
        Diagnostics::LogLine("info", "world_presence", inWorld ? "in world" : "out of world (main menu button shown)");
    }
    const bool menuDown = gameFocused
        && (GetAsyncKeyState(static_cast<int>(configuredMenuKey.load())) & 0x8000) != 0;
    // While a replay plays, K opens its editor; the editor's gear button
    // switches to the ClickGUI.
    bool replayRunning = ReplayInjector::Running();
    // Only gameplay (cursor hidden) or the pause screen may open the menu: a
    // visible cursor otherwise means a game screen such as chat is up, and K
    // typed into chat must not toggle the overlay.
    const bool menuKeyAllowed = menuVisible || (inWorld && (!cursorVisible || minecraftPauseVisible));
    if (menuDown && !menuKeyDown && menuKeyAllowed) {
        SetMenuVisible(!menuVisible, replayRunning ? MenuKind::ReplayEditor : MenuKind::ClickGui);
    }
    menuKeyDown = menuDown;
    // The replay hotbar takes the wheel and left click while the replay is
    // being watched with the camera (no menu, no pause screen).
    const bool hotbar = replayRunning && gameFocused && levelRendered && !menuVisible && !cursorVisible;
    if (hotbar != replayHotbarActive.load(std::memory_order_acquire)) {
        replayHotbarActive.store(hotbar, std::memory_order_release);
        MouseInputBlocker::SetReplayHotbar(hotbar);
        replayHotbarScroll.store(0, std::memory_order_release);
        replayHotbarClicks.store(0, std::memory_order_release);
        replayHotbarRightClicks.store(0, std::memory_order_release);
    }
    // Without the mouse feed hook, clicks are polled (the game sees them too,
    // which spectator mode ignores).
    const bool leftDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    if (hotbar && leftDown && !replayHotbarLeftDown && !MouseInputBlocker::ReplayHotbarCaptured()) {
        replayHotbarClicks.fetch_add(1, std::memory_order_acq_rel);
    }
    replayHotbarLeftDown = leftDown;
    const bool rightDown = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
    if (hotbar && rightDown && !replayHotbarRightDown && !MouseInputBlocker::ReplayHotbarCaptured()) {
        replayHotbarRightClicks.fetch_add(1, std::memory_order_acq_rel);
    }
    replayHotbarRightDown = rightDown;
    // Joining the replay world opens the editor once per replay.
    if (!replayRunning) {
        replayEditorAutoOpened = false;
        replayWorldRendered = false;
    } else if (levelRendered) {
        replayWorldRendered = true;
    }
    if (replayRunning && !replayEditorAutoOpened && levelRendered && gameFocused && !menuVisible
        && ReplayInjector::CurrentStatus().playing) {
        replayEditorAutoOpened = true;
        Diagnostics::LogLine("info", "replay_editor", "replay world joined; opening the editor");
        SetMenuVisible(true, MenuKind::ReplayEditor);
    }
    ApplyKeyHoldAssists(gameFocused, cursorVisible);
    ApplyGameplayInput(gameFocused, cursorVisible);
    ApplyFramePacing(gameFocused);
    const auto presentedAt = std::chrono::steady_clock::now();
    const bool gameplayHudVisible = GameplayHudVisible(cursorVisible, gameFocused, presentedAt);
    UpdateMinecraftPauseVisible(cursorVisible, gameFocused, presentedAt);
    const float frameSeconds = lastPresentAt == std::chrono::steady_clock::time_point{} ? 0.0F
        : std::chrono::duration<float>(presentedAt - lastPresentAt).count();
    lastPresentAt = presentedAt;
    ++frameCounter;
    if (menuVisible) {
        const auto now = std::chrono::steady_clock::now();
        if (lastDiagnosticLog == std::chrono::steady_clock::time_point{}) lastDiagnosticLog = now;
        if (now - lastDiagnosticLog >= std::chrono::seconds(1)) {
            lastDiagnosticLog = now;
            POINT cursor{};
            GetCursorPos(&cursor);
            Diagnostics::LogLine("info", "mouse_isolation_diagnostic", std::format(
                "menuVisible=1 mouseMsgsToGameWindow={} rawInputMsgsToGameWindow={} keyMsgsToGameWindow={} "
                "mouseMsgsToOtherWindows={} keyMsgsToOtherWindows={} rawMousePacketsBlocked={} screenCursor=({},{})",
                mouseMessagesWhileMenu.exchange(0, std::memory_order_relaxed),
                rawInputMessagesWhileMenu.exchange(0, std::memory_order_relaxed),
                keyboardMessagesWhileMenu.exchange(0, std::memory_order_relaxed),
                extraWindowMouseMessages.exchange(0, std::memory_order_relaxed),
                extraWindowKeyMessages.exchange(0, std::memory_order_relaxed),
                rawMousePacketsBlocked.exchange(0, std::memory_order_relaxed),
                cursor.x, cursor.y));
        }
    }
    // moduleManager must be non-null for the draw path below (OverlayUi::Draw
    // dereferences it); the transient-UI case used to fall through without it,
    // which a Present landing after Remove() cleared it could reach while a
    // toast was still on screen.
    if (!moduleManager) {
        overlayControlsVisible.store(false, std::memory_order_release);
        return originalPresent(baseSwapChain, interval, flags);
    }
    const bool showReplayPanel = minecraftPauseVisible
        && (!moduleManager->EnabledModulesOf<IReplayControlModule>().empty() || ReplayInjector::Running());
    // Only a positively observed title screen may show the button. Treating
    // unknown telemetry as the title caused a short-lived Replays button to
    // appear wherever injection happened until the first screen event arrived.
    const bool onTitleScreen = ScreenTelemetry::Known() && ScreenTelemetry::OnTitleScreen();
    const bool onDisconnectedScreen = ScreenTelemetry::Known() && ScreenTelemetry::OnDisconnectedScreen();
    // Save & Quit tears down the local connection without necessarily giving
    // the packet hook another session start. Once the title is confirmed and
    // the replay world has stopped rendering, retire playback so its bottom
    // bar and input capture cannot leak onto the menus.
    if (replayRunning && worldKnown && !levelRendered
        && ((replayWorldRendered && onTitleScreen) || onDisconnectedScreen)
        && ReplayInjector::CurrentStatus().inWorld) {
        ReplayInjector::Stop();
        replayRunning = false;
        replayEditorAutoOpened = false;
        replayWorldRendered = false;
        Diagnostics::LogLine("info", "replay_world_exit",
            onDisconnectedScreen ? "replay world disconnected; playback reset"
                                 : "title screen observed after the replay world closed; playback reset");
    }
    const bool showReplaysButton = !levelRendered && onTitleScreen && gameFocused && !menuVisible;
    overlayControlsVisible.store(showReplayPanel || showReplaysButton, std::memory_order_release);
    if (replayRunning || ReplayWorld::Pending()) {
        replayWorldCleanupDue = true;
    } else if (replayWorldCleanupDue && worldKnown && !levelRendered && onTitleScreen) {
        replayWorldCleanupDue = false;
        ReplayWorld::DeleteReplayWorldsAsync([] { return ReplayInjector::Running() || ReplayWorld::Pending(); });
    }
    if (controlsBlockingMouse && !menuVisible && !showReplayPanel && !showReplaysButton) {
        controlsBlockingMouse = false;
        MouseInputBlocker::SetActive(false);
    }
    if (!menuVisible && !showReplayPanel && !showReplaysButton && !moduleManager->HasEnabledOverlayModules() && !OverlayUi::HasTransientUi()) {
        frameBlend.Invalidate();
        return originalPresent(baseSwapChain, interval, flags);
    }
    {
        // Scoped so the lock is released before originalPresent: holding it
        // across Present blocks for a full vsync, and HookResize (window
        // thread) and UpdatePlayerSkin (scan worker) both wait on it.
        std::scoped_lock lock(stateMutex);
        ComPtr<IDXGISwapChain3> swapChain;
        if (SUCCEEDED(baseSwapChain->QueryInterface(IID_PPV_ARGS(&swapChain)))) {
            if (!imguiReady) (void)InitializeRenderer(swapChain.Get());
            auto* queue = commandQueue.load(std::memory_order_acquire);
            if (imguiReady && queue && !frames.empty()) {
                const UINT index = swapChain->GetCurrentBackBufferIndex();
                auto& frame = frames.at(index);
                bool framePresentable = true;
                if (frame.fenceValue != 0 && renderFence->GetCompletedValue() < frame.fenceValue) {
                    framePresentable = SUCCEEDED(renderFence->SetEventOnCompletion(frame.fenceValue, fenceEvent))
                        && WaitForSingleObject(fenceEvent, 250) == WAIT_OBJECT_0;
                }
                if (framePresentable
                    && SUCCEEDED(frame.allocator->Reset())
                    && SUCCEEDED(commandList->Reset(frame.allocator.Get(), nullptr))) {
                    // Frame blending: only while gameplay is visible (never
                    // over the menu), and only with a valid previous capture.
                    float blendStrength = 0.0F;
                    if (!menuVisible && !cursorVisible) {
                        for (const auto* module : moduleManager->EnabledModulesOf<IFrameBlendModule>()) {
                            blendStrength = std::max(blendStrength, std::clamp(module->BlendStrength(), 0.0F, MaxFrameBlendStrength));
                        }
                    }
                    // The replay editor shows the game in its viewport from
                    // the same raw-frame capture frame blending uses.
                    const bool replayEditor = menuVisible && menuIsReplayEditor.load(std::memory_order_acquire);
                    // Colour grading: over gameplay and game screens, never
                    // over the menu.
                    bool postActive = false;
                    if (!menuVisible) {
                        const auto graders = moduleManager->EnabledModulesOf<IPostProcessModule>();
                        if (!graders.empty()) {
                            framePostProcess = graders.front()->PostProcess();
                            postActive = !framePostProcess.Identity();
                        }
                    }
                    postActive = postActive && postProcess.Ensure(device.Get(), renderFormat);
                    bool captureFrame = false;
                    std::optional<D3D12_GPU_DESCRIPTOR_HANDLE> previousFrame;
                    std::optional<D3D12_GPU_DESCRIPTOR_HANDLE> currentFrame;
                    if (blendStrength > 0.0F || replayEditor || postActive) {
                        const UINT increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
                        auto cpu = srvHeap->GetCPUDescriptorHandleForHeapStart();
                        auto gpu = srvHeap->GetGPUDescriptorHandleForHeapStart();
                        cpu.ptr += static_cast<SIZE_T>(increment) * FontDescriptorCount;
                        gpu.ptr += static_cast<UINT64>(increment) * FontDescriptorCount;
                        captureFrame = frameBlend.Ensure(device.Get(), frame.backBuffer->GetDesc(), cpu, gpu, increment);
                        if (captureFrame) {
                            previousFrame = frameBlend.PreviousFrame();
                            currentFrame = frameBlend.CurrentFrame();
                        }
                    }
                    ImGui_ImplDX12_NewFrame();
                    {
                        std::scoped_lock imguiLock(imguiInputMutex);
                        ImGui_ImplWin32_NewFrame();
                        UpdateImGuiMouseFromSystem();
                        ImGui::NewFrame();
                    }
                    const bool replayTheater = menuVisible && menuIsReplayTheater.load(std::memory_order_acquire);
                    // The theater opens over Minecraft's own menus, whose
                    // system cursor is already showing.
                    ImGui::GetIO().MouseDrawCursor = menuVisible && !replayTheater;
                    if (currentFrame && postActive) {
                        auto* background = ImGui::GetBackgroundDrawList();
                        background->AddCallback(&PostProcessCallback, nullptr);
                        background->AddImage(reinterpret_cast<ImTextureID>(currentFrame->ptr),
                            ImVec2(0.0F, 0.0F), ImGui::GetIO().DisplaySize);
                        background->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
                    }
                    if (previousFrame && blendStrength > 0.0F) {
                        const auto alpha = static_cast<int>(std::lround(blendStrength * 255.0F));
                        ImGui::GetBackgroundDrawList()->AddImage(reinterpret_cast<ImTextureID>(previousFrame->ptr),
                            ImVec2(0.0F, 0.0F), ImGui::GetIO().DisplaySize, ImVec2(0.0F, 0.0F), ImVec2(1.0F, 1.0F),
                            IM_COL32(255, 255, 255, alpha));
                    }
                    OverlayUi::FrameInfo frameInfo{frameSeconds, frameCounter, presentedAt, gameFocused, {}, {}};
                    if (replayEditor && previousFrame) frameInfo.gameFrameTexture = previousFrame->ptr;
                    if (replayHotbarActive.load(std::memory_order_acquire)) {
                        frameInfo.replayHotbarScroll = replayHotbarScroll.exchange(0, std::memory_order_acq_rel);
                        frameInfo.replayHotbarClicks = replayHotbarClicks.exchange(0, std::memory_order_acq_rel);
                        frameInfo.replayHotbarRightClicks = replayHotbarRightClicks.exchange(0, std::memory_order_acq_rel);
                    }
                    if (gameFocused) {
                        std::bitset<256> keys;
                        for (int key = 1; key < 255; ++key) keys[static_cast<std::size_t>(key)] = (GetAsyncKeyState(key) & 0x8000) != 0;
                        frameInfo.keysDown = keys;
                    }
                    const OverlayUi::DrawMode mode{menuVisible, replayTheater, gameplayHudVisible,
                        showReplayPanel, showReplaysButton, replayEditor};
                    const auto draw = OverlayUi::Draw(*moduleManager, mode, hudEditMode, frameInfo);
                    if (draw.settingsDirty) settingsDirty.store(true);
                    if (draw.closeRequested) SetMenuVisible(false);
                    if (draw.openReplayTheater && !menuVisible) SetMenuVisible(true, MenuKind::ReplayTheater);
                    if (draw.openClickGui && menuVisible) menuIsReplayEditor.store(false, std::memory_order_release);
                    if (draw.openReplayEditor && !menuVisible) SetMenuVisible(true, MenuKind::ReplayEditor);
                    // Minecraft may read menu clicks through GameInput, which
                    // the window-message filter cannot stop; block it while
                    // the pointer is over one of our controls.
                    if (!menuVisible) {
                        const bool block = (showReplayPanel || showReplaysButton) && ImGui::GetIO().WantCaptureMouse;
                        if (block != controlsBlockingMouse) {
                            controlsBlockingMouse = block;
                            MouseInputBlocker::SetActive(block);
                        }
                    } else {
                        controlsBlockingMouse = false;
                    }
                    ImGui::Render();
                    // A video recording copies the raw game frame as well
                    // (before any overlay); the back buffer stays in PRESENT.
                    videoCapture.Collect(renderFence->GetCompletedValue());
                    videoCapture.Record(device.Get(), commandList.Get(), frame.backBuffer.Get(), presentedAt);
                    D3D12_RESOURCE_BARRIER barrier{};
                    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                    barrier.Transition.pResource = frame.backBuffer.Get();
                    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
                    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
                    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                    // The capture copies the raw game frame (before any
                    // overlay) and leaves the backbuffer in RENDER_TARGET.
                    if (captureFrame) {
                        frameBlend.RecordCapture(commandList.Get(), frame.backBuffer.Get());
                    } else {
                        frameBlend.Invalidate();
                        commandList->ResourceBarrier(1, &barrier);
                    }
                    commandList->OMSetRenderTargets(1, &frame.rtv, FALSE, nullptr);
                    ID3D12DescriptorHeap* heaps[]{ srvHeap.Get() };
                    commandList->SetDescriptorHeaps(1, heaps);
                    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), commandList.Get());
                    std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
                    commandList->ResourceBarrier(1, &barrier);
                    if (SUCCEEDED(commandList->Close())) {
                        ID3D12CommandList* lists[]{ commandList.Get() };
                        queue->ExecuteCommandLists(1, lists);
                        const UINT64 signalValue = nextFenceValue++;
                        if (SUCCEEDED(queue->Signal(renderFence.Get(), signalValue))) {
                            frame.fenceValue = signalValue;
                            videoCapture.Submitted(signalValue);
                        }
                    }
                }
            }
        }
    }
    return originalPresent(baseSwapChain, interval, flags);
}

// DXGI fails a resize while any reference to a back buffer is outstanding, and
// Minecraft aborts the process when its resize fails. Before forwarding either
// resize entry point, drop every back-buffer reference we hold.
//
// The common case (same buffer count and format, i.e. dragging the window
// edge) only swaps the back buffers and keeps ImGui, the window subclass and
// the device objects alive. Tearing all of that down and rebuilding it on the
// next Present, dozens of times during one drag, is what this used to do.
bool PrepareForResize(UINT count, DXGI_FORMAT format) noexcept {
    const bool lightweight = imguiReady && !frames.empty()
        && (count == 0 || count == frames.size())
        && (format == DXGI_FORMAT_UNKNOWN || format == renderFormat);
    if (!lightweight) {
        ReleaseRenderState();
        return false;
    }
    WaitForOverlayGpuIdle();
    frameBlend.Release();
    videoCapture.Release();
    for (auto& frame : frames) {
        frame.backBuffer.Reset();
        frame.fenceValue = 0;
    }
    return true;
}

void RebindBackBuffers(IDXGISwapChain* swapChain) noexcept {
    bool rebound = true;
    for (UINT index = 0; index < frames.size() && rebound; ++index) {
        rebound = SUCCEEDED(swapChain->GetBuffer(index, IID_PPV_ARGS(&frames[index].backBuffer)));
        if (rebound) device->CreateRenderTargetView(frames[index].backBuffer.Get(), nullptr, frames[index].rtv);
    }
    if (!rebound) {
        Diagnostics::LogLine("warning", "overlay_resize",
            "back buffers could not be re-acquired after resize; rebuilding the overlay");
        ReleaseRenderState();
    }
}

HRESULT STDMETHODCALLTYPE HookResize(IDXGISwapChain* swapChain, UINT count, UINT width, UINT height, DXGI_FORMAT format, UINT flags) {
    const CallbackGuard callback;
    std::scoped_lock lock(stateMutex);
    const bool rebind = PrepareForResize(count, format);
    const HRESULT result = originalResize(swapChain, count, width, height, format, flags);
    if (rebind) {
        if (SUCCEEDED(result)) RebindBackBuffers(swapChain);
        else ReleaseRenderState();
    }
    return result;
}

HRESULT STDMETHODCALLTYPE HookResize1(IDXGISwapChain3* swapChain, UINT count, UINT width, UINT height,
    DXGI_FORMAT format, UINT flags, const UINT* nodeMasks, IUnknown* const* presentQueues) {
    const CallbackGuard callback;
    std::scoped_lock lock(stateMutex);
    const bool rebind = PrepareForResize(count, format);
    const HRESULT result = originalResize1(swapChain, count, width, height, format, flags, nodeMasks, presentQueues);
    if (rebind) {
        if (SUCCEEDED(result)) RebindBackBuffers(swapChain);
        else ReleaseRenderState();
    }
    return result;
}

LRESULT CALLBACK DummyWindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    return DefWindowProcW(window, message, wParam, lParam);
}
}

bool Install(ModuleManager& modules, std::wstring& error) noexcept {
    if (installed.exchange(true)) return true;
    moduleManager = &modules;
    if (!pacingWakeEvent) pacingWakeEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (pacingWakeEvent) ResetEvent(pacingWakeEvent);
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = DummyWindowProcedure;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = L"Oinkster.Dx12Probe";
    RegisterClassW(&windowClass);
    HWND window = CreateWindowExW(0, windowClass.lpszClassName, L"", WS_OVERLAPPED, 0, 0, 8, 8, nullptr, nullptr, windowClass.hInstance, nullptr);
    ComPtr<ID3D12Device> probeDevice;
    ComPtr<ID3D12CommandQueue> probeQueue;
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGISwapChain1> probeSwapChain;
    bool success = false;
    if (window && SUCCEEDED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&probeDevice)))) {
        D3D12_COMMAND_QUEUE_DESC queueDescription{};
        queueDescription.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (SUCCEEDED(probeDevice->CreateCommandQueue(&queueDescription, IID_PPV_ARGS(&probeQueue)))
            && SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
            DXGI_SWAP_CHAIN_DESC1 swapDescription{};
            swapDescription.Width = 8;
            swapDescription.Height = 8;
            swapDescription.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            swapDescription.SampleDesc.Count = 1;
            swapDescription.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            swapDescription.BufferCount = 2;
            swapDescription.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
            if (SUCCEEDED(factory->CreateSwapChainForHwnd(probeQueue.Get(), window, &swapDescription, nullptr, nullptr, &probeSwapChain))) {
                void** swapTable = *reinterpret_cast<void***>(probeSwapChain.Get());
                void** queueTable = *reinterpret_cast<void***>(probeQueue.Get());
                const auto status = MH_Initialize();
                if (status == MH_OK || status == MH_ERROR_ALREADY_INITIALIZED) {
                    void* executeTarget = queueTable[10];
                    void* presentTarget = swapTable[8];
                    void* resizeTarget = swapTable[13];
                    success = HookManager::Create(executeTarget, reinterpret_cast<void*>(HookExecute),
                                  reinterpret_cast<void**>(&originalExecute))
                        && HookManager::Create(presentTarget, reinterpret_cast<void*>(HookPresent),
                            reinterpret_cast<void**>(&originalPresent))
                        && HookManager::Create(resizeTarget, reinterpret_cast<void*>(HookResize),
                            reinterpret_cast<void**>(&originalResize));
                    // IDXGISwapChain3::ResizeBuffers1 (slot 39) is a separate
                    // entry point. Without this hook a game resizing through
                    // it would find our back-buffer references still alive.
                    ComPtr<IDXGISwapChain3> probeSwapChain3;
                    if (success && SUCCEEDED(probeSwapChain.As(&probeSwapChain3))) {
                        void** swap3Table = *reinterpret_cast<void***>(probeSwapChain3.Get());
                        void* resize1Target = swap3Table[39];
                        if (resize1Target != resizeTarget
                            && !HookManager::Create(resize1Target, reinterpret_cast<void*>(HookResize1),
                                reinterpret_cast<void**>(&originalResize1))) {
                            originalResize1 = nullptr;
                            Diagnostics::LogLine("warning", "overlay_resize",
                                "IDXGISwapChain3::ResizeBuffers1 hook failed to install");
                        }
                    }
                }
            }
        }
    }
    if (window) DestroyWindow(window);
    UnregisterClassW(windowClass.lpszClassName, windowClass.hInstance);
    if (!success) {
        error = L"Direct3D 12 hook validation failed; no overlay was installed.";
        Remove();
    } else {
        if (!HookManager::Create(reinterpret_cast<void*>(&ClipCursor), reinterpret_cast<void*>(HookClipCursor),
            reinterpret_cast<void**>(&originalClipCursor))) {
            Diagnostics::LogLine("warning", "clip_cursor_hook",
                "ClipCursor hook failed to install; the game may fight the overlay for cursor control");
        }
        if (!InstallRawInputDataImportHook()) {
            Diagnostics::LogLine("warning", "raw_input_hook",
                "Minecraft GetRawInputData import slot was not found; falling back to exported function hook");
            if (!HookManager::Create(reinterpret_cast<void*>(&GetRawInputData), reinterpret_cast<void*>(HookGetRawInputData),
                reinterpret_cast<void**>(&originalGetRawInputData))) {
                Diagnostics::LogLine("warning", "raw_input_hook", "GetRawInputData fallback hook failed to install");
            }
        }
        if (!HookManager::Create(reinterpret_cast<void*>(&GetRawInputBuffer), reinterpret_cast<void*>(HookGetRawInputBuffer),
            reinterpret_cast<void**>(&originalGetRawInputBuffer))) {
            Diagnostics::LogLine("warning", "raw_input_hook", "GetRawInputBuffer hook failed to install");
        }
        cursorVisibilityInitialized = false;
        pauseProbeRequested.store(false, std::memory_order_release);
        overlayControlsVisible.store(false, std::memory_order_release);
        menuIsReplayTheater.store(false, std::memory_order_release);
        menuIsReplayEditor.store(false, std::memory_order_release);
        replayHotbarActive.store(false, std::memory_order_release);
        replayEditorAutoOpened = false;
        replayWorldRendered = false;
        controlsBlockingMouse = false;
        loggedInWorld = true;
        minecraftPauseVisible = false;
        pauseCandidateUntil = {};
        pauseCursorHiddenSince = {};
        previousFrameCursorVisible = false;
        OverlayUi::BeginSession();
    }
    return success;
}

bool UpdatePlayerSkin(std::uint32_t width, std::uint32_t height,
    std::span<const std::uint8_t> rgbaPixels, std::uint64_t revision) noexcept {
    if ((width != 64 || height != 64) && (width != 128 || height != 128)) {
        Diagnostics::LogLine("warn", "player_skin_renderer", std::format(
            "head texture rejected: revision={:#x} unsupported atlas={}x{}", revision, width, height));
        return false;
    }
    const std::size_t expected = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4;
    if (rgbaPixels.size() != expected || revision == 0) {
        Diagnostics::LogLine("warn", "player_skin_renderer", std::format(
            "head texture rejected: revision={:#x} bytes={} expected={}", revision, rgbaPixels.size(), expected));
        return false;
    }
    if (!OverlayUi::UpdatePlayerSkin(width, height, rgbaPixels, revision)) {
        Diagnostics::LogLine("warn", "player_skin_renderer", std::format(
            "head texture rejected by UI extractor: revision={:#x}", revision));
        return false;
    }
    // OverlayUi::UpdatePlayerSkin above owns the head texture now. This used to
    // additionally flatten the atlas into a local 8x8 playerHead array for an
    // earlier 2D head renderer that no longer exists - taking stateMutex (which
    // the Present hook holds for the whole draw) once per skin delivery to
    // produce pixels nothing read.
    if (revision == playerHeadRevision.exchange(revision, std::memory_order_acq_rel)) return true;
    Diagnostics::LogLine("info", "player_skin_renderer", std::format(
        "head texture accepted for render: revision={:#x} atlas={}x{}", revision, width, height));
    return true;
}

bool ConsumeSettingsDirty() noexcept { return settingsDirty.exchange(false); }

bool ConsumeEjectRequested() noexcept { return ejectRequested.exchange(false); }

void ConfigureControlKeys(std::uint32_t menuKey, std::uint32_t unloadKey) noexcept {
    if (menuKey > 0 && menuKey < 256 && unloadKey > 0 && unloadKey < 256 && menuKey != unloadKey) {
        configuredMenuKey.store(menuKey);
        configuredUnloadKey.store(unloadKey);
        // Keep the isolation tracker's idea of "not gameplay input" in step
        // with the configured controls.
        std::scoped_lock keyLock(keyStateMutex);
        keyStateTracker.ConfigureControlKeys(menuKey, unloadKey);
    }
}

std::uint32_t MenuKey() noexcept { return configuredMenuKey.load(); }
std::uint32_t UnloadKey() noexcept { return configuredUnloadKey.load(); }

void QueueReplayHotbarScroll(int notches) noexcept { replayHotbarScroll.fetch_add(notches, std::memory_order_acq_rel); }

void QueueReplayHotbarClick() noexcept { replayHotbarClicks.fetch_add(1, std::memory_order_acq_rel); }

void QueueReplayHotbarRightClick() noexcept { replayHotbarRightClicks.fetch_add(1, std::memory_order_acq_rel); }

void QueueMouseWheel(float delta) noexcept {
    extraVerticalWheel.fetch_add(static_cast<int>(delta * static_cast<float>(WHEEL_DELTA)), std::memory_order_relaxed);
}

bool IsMenuVisible() noexcept { return menuVisible.load(std::memory_order_relaxed); }

void Remove() noexcept {
    if (!installed.exchange(false)) return;
    // Wake a render thread parked in a dynamic-FPS wait before draining.
    if (pacingWakeEvent) SetEvent(pacingWakeEvent);
    menuVisible.store(false, std::memory_order_release);
    pauseProbeRequested.store(false, std::memory_order_release);
    overlayControlsVisible.store(false, std::memory_order_release);
    menuIsReplayTheater.store(false, std::memory_order_release);
    menuIsReplayEditor.store(false, std::memory_order_release);
    replayHotbarActive.store(false, std::memory_order_release);
    replayEditorAutoOpened = false;
    replayWorldRendered = false;
    MouseInputBlocker::SetReplayHotbar(false);
    controlsBlockingMouse = false;
    loggedInWorld = true;
    minecraftPauseVisible = false;
    pauseCandidateUntil = {};
    pauseCursorHiddenSince = {};
    previousFrameCursorVisible = false;
    ReleaseAssistKeys();
    MouseInputBlocker::SetActive(false);
    RestoreRawInputDataImport();
    HookManager::DisableAll();
    for (int attempt = 0; attempt < 200 && activeCallbacks.load(std::memory_order_acquire) != 0; ++attempt) Sleep(5);
    if (activeCallbacks.load(std::memory_order_acquire) != 0) {
        // A thread is still inside one of our detours. Removing the hooks now
        // would free a trampoline it is executing, so keep the image mapped.
        Diagnostics::LogLine("warning", "overlay_remove",
            "a thread was still inside a Direct3D detour; leaving the hooks installed");
        UnloadGuard::Block("a Direct3D detour still had a live thread at detach");
    } else {
        HookManager::RemoveAll();
        (void)MH_Uninitialize();
        // No detour can run any more, so nothing can wait on the event.
        if (pacingWakeEvent) {
            CloseHandle(pacingWakeEvent);
            pacingWakeEvent = nullptr;
        }
    }
    std::scoped_lock lock(stateMutex);
    ReleaseRenderState();
    if (auto* queue = commandQueue.exchange(nullptr, std::memory_order_acq_rel)) queue->Release();
    originalPresent = nullptr;
    originalResize = nullptr;
    originalResize1 = nullptr;
    originalExecute = nullptr;
    originalClipCursor = nullptr;
    originalGetRawInputData = nullptr;
    originalGetRawInputBuffer = nullptr;
    rawInputDataImportSlot = nullptr;
    moduleManager = nullptr;
    menuKeyDown = false;
    unloadKeyDown = false;
    cursorVisibilityInitialized = false;
    menuVisible.store(false, std::memory_order_release);
    hudEditMode = false;
    playerHeadRevision.store(0, std::memory_order_release);
    settingsDirty.store(false);
    ejectRequested.store(false);
    extraVerticalWheel.store(0);
    extraHorizontalWheel.store(0);
    heldAssistKeys.clear();
    frameCounter = 0;
    lastPresentAt = {};
    // If a detour was still live, pacingWakeEvent was deliberately left open
    // and signalled so that thread can never wait on a closed handle.
}

}
