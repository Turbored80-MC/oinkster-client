# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this repository is

Oinkster is a Minecraft Bedrock (Windows) client-side mod/"fair-play tools" project, made of two coupled pieces that build and test independently:

- **Launcher** (repo root, C# / WinUI 3, .NET 10): `App.xaml(.cs)`, `MainWindow.xaml(.cs)`, `MainPage.xaml(.cs)`, plus `Services/`, `Models/`, `ViewModels/`. Installs/updates the native client, verifies and injects it into a running `Minecraft.Windows` process, and surfaces status/settings.
- **Native client** (`Native/`, C++23): a DLL that gets injected into the Minecraft process. It hooks the DX12 swapchain and renders a Dear ImGui overlay (the in-game ClickGUI, opened with K) directly into the game's own backbuffer. This is where almost all actual "mod" behavior lives.

There is no top-level `.sln`; the two sides are built with separate toolchains (`dotnet` and `cmake`) and talk to each other only via the injection handshake and the pipes described below — not via shared project references (except `OinksterClient.Tests`, which links a few `Services`/`Models` files directly).

## Build, run, test

### Launcher (run from repo root, PowerShell)
```powershell
dotnet restore
dotnet build OinksterClient.csproj -p:Platform=x64
dotnet run --project OinksterClient.csproj -p:Platform=x64
```
Requires Visual Studio 2022 with the WinUI application development workload for XAML debugging. Both C# projects set `TreatWarningsAsErrors` **for Debug only**, so a warning fails the normal dev build the same way `/W4 /WX` does natively. Release is deliberately **not trimmed**: trimming disables reflection-based `System.Text.Json`, which every settings, catalog and manifest read uses, and the shipped launcher fails at runtime ("Reflection-based serialization has been disabled"). Release is unpackaged (`WindowsPackageType=None`) and self-contained; `Tools/New-Installer.ps1` wraps it in the Inno Setup installer. Service and model logic is covered by `OinksterClient.Tests` (below); the WinUI layer itself has no automated coverage, so changes there still need a manual smoke test (Play, Settings, Back, resizing, keyboard focus, asset loading).

### Launcher unit tests (`OinksterClient.Tests`, xUnit)
```powershell
dotnet test OinksterClient.Tests/OinksterClient.Tests.csproj
```
Only a few `Services`/`Models` files are linked into this project (see its `.csproj`); it has no WinUI dependency. Name new tests `MethodName_Condition_ExpectedResult`.

### Native client (`Native/`)
```powershell
cmake --preset windows-x64
cmake --build --preset debug
ctest --preset debug
```
Requires VS2022 "Desktop development with C++", the Windows 10/11 SDK, and CMake 3.24+. If `cmake`/`cl` aren't on `PATH` in the shell you're using, invoke the CMake bundled with Visual Studio directly, e.g.:
```powershell
& "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --build "Native\out\build\x64" --target OinksterClient --config Debug
```
Build output lands in `Native/out/build/x64/<Config>/`: `oinkster-client-x64.dll` (the injected client), `Oinkster.ModuleHarness.exe`, `Oinkster.Native.Tests.exe`, `Oinkster.Dx12Hook.Tests.exe`, `Oinkster.Injector.exe`. The `OinksterCore` target is built with `/W4 /WX` (warnings are errors) — a change that introduces any warning fails the build. To run a single native test binary directly instead of through ctest: `& "Native\out\build\x64\Debug\Oinkster.Native.Tests.exe"`.

In Debug builds, `OinksterClient.csproj` automatically copies the freshly built `oinkster-client-x64.dll` into the launcher's output (and into its AppX layout) — so after rebuilding the native DLL, rebuilding the launcher picks up the new binary without any manual copy step.

### Diagnostics while testing live
The native client logs JSON lines to `%LOCALAPPDATA%\Oinkster\Client\oinkster-native.log` via `Diagnostics::LogLine` (`Native/src/Diagnostics.cpp`). This is the first place to look when something can't be reverse-engineered or verified statically (e.g. memory-read offsets that only make sense once seen against a live game).

## Native client architecture

- **Entry/lifecycle**: `DllMain.cpp` → `Runtime` (`Runtime.hpp/.cpp`) owns the `ModuleManager`, the `IBedrockAdapter`, a `StatusPipe` (reports status to the launcher) and a `ControlPipe` (receives reload requests from the launcher).
- **Game attachment boundary**: `IBedrockAdapter` / `ReviewedBedrockAdapter` (`BedrockAdapter.hpp/.cpp`) is the sanctioned place to identify a game build and attach rendering/event hooks; it validates signatures and must fully detach on failure. In practice, some native code reads game-process memory directly outside this boundary for specific features (see `PlayerSkinTelemetry.cpp` below) — be aware the "modules only get read-only snapshots" framing in `Native/README.md` is the design goal, not a hard guarantee enforced everywhere in the current code.
- **Hooking**: `HookManager.hpp/.cpp` is the single MinHook-based registry — hooks are created/disabled/removed through it so everything tears down together, in creation order. `Dx12Overlay.cpp` is the main consumer: it hooks `IDXGISwapChain::Present`/`ResizeBuffers` and the command queue's `ExecuteCommandLists` to get a render hook and a `D3D12` device/queue.
- **Rendering / ClickGUI**: `Dx12Overlay.cpp` initializes Dear ImGui against the game's DX12 device and calls `OverlayUi::Draw(...)` every frame. **`Native/src/OverlayUi.cpp` is the live ClickGUI implementation** — K opens a home hub (rotating skin head over HUD layout / MODS / Settings tiles); each tile opens one window with a sidebar (Home, views, categories, HUD layout, Settings) and an expandable module list where each row opens its own settings inline. Esc steps back to the hub, then closes. `ClickGui.cpp`/`ClickGui.hpp` and the Direct2D-based `Renderer.cpp`/`Renderer.hpp` are an earlier, vestigial implementation that only draws a blank placeholder panel — do not extend those for UI work; extend `OverlayUi.cpp`.
- **Menu/unload keybinds**: polled each `Present` call in `Dx12Overlay.cpp`'s `HookPresent` via `GetAsyncKeyState` against `configuredMenuKey`/`configuredUnloadKey` (default menu key `K`). The runtime deliberately never installs a global keyboard/mouse hook; while the menu is open, input isolation is scoped to Minecraft's own window(s) and its `GameInputRedist` mouse-state call (`MouseInputBlocker.cpp`, `InputState.hpp`) — preserve that scoping if you touch input handling.
- **Modules**: `Module.hpp` defines `IModule`/`IHudModule` (metadata incl. `requiredCapabilities`, typed `SettingDescriptor`s, `HudContent`, actions, typed event callbacks) plus integration mixins (`IOverlayModule`, `IPresentationModule`, `IFrameBlendModule`, `IFramePacingModule`, `ISprintAssistModule`, `IKeyHoldAssistModule`) — integration code finds modules by interface, never by id. The 70 implementations live in `Native/src/Modules/*Modules.cpp` (one file per category) with pure, unit-tested logic in `include/Oinkster/Modules/*Logic.hpp`. `ModuleManager` owns lifecycle, dispatch, keybinds, settings normalization and **capability-based availability** (`SetCapability`; see `Capabilities.hpp`). `ModuleCatalog.cpp` is the native catalog table; `Assets/module-catalog.json` is the launcher copy, and `Oinkster.Native.Tests` fails on any drift between them — when adding or changing a module, update both. Settings keys and metric definitions are documented in `Native/README.md`.
- **Supported build**: named once in `include/Oinkster/SupportedBuild.hpp`. Signature scans must use `SignatureScan::FindUniqueInText` (exactly one match across all candidate patterns, else fail closed).
- **Skin/head rendering**: `PlayerSkinTelemetry.cpp` reads the local player's currently-equipped skin live out of the Minecraft process's own memory — via a `libhat` pattern scan to locate a vtable offset, then a chain of reverse-engineered struct offsets (player map → player-list entry → skin owner → image blob) documented inline as release-build (not debug-STL) layouts. There is intentionally no disk-based skin loading: Bedrock has no single reliable on-disk file for "the skin currently equipped" (marketplace/persona skins are encrypted and account/Xbox-Live-synced), so this is the only robust source. `SkinHead.cpp` extracts the 6 head faces from the classic Minecraft skin UV atlas (64×64 or 128×128 RGBA) into a `SkinHeadTexture`; `OverlayUi.cpp`'s `DrawSkinHead`/`DrawCubeLayer` render it as a small rotating 3D cube that eases toward the mouse cursor via `StepCriticallyDamped` (`SkinHead.cpp`) — a critically-damped spring with a frame-time clamp whose exact cap is pinned by a unit test in `NativeTests.cpp`, so don't change that constant without updating the test's expectation deliberately.
- **Settings/persistence**: `SettingsStore.cpp` parses settings.json with the bounded `Json.hpp` reader and persists module enablement (the *requested* state, so modules waiting on a capability survive restarts), keybinds, HUD layouts and module settings; `ApplySettings`/`CaptureSettings` are the single path between the store and `ModuleManager`. `Runtime` debounces saves and flushes on unload; `ControlPipe` lets the launcher trigger a reload without re-injecting.

## Launcher ↔ native handshake

`Services/ReviewedClientInjector.cs` is the only path that activates the native DLL, and it's deliberately paranoid: it verifies the DLL's filename and SHA-256 against a signed release manifest, confirms the target process is actually `Minecraft.Windows` running as native x64, confirms the Bedrock package version string matches the DLL's supported version, and only then does the classic `VirtualAllocEx` + `WriteProcessMemory` + `CreateRemoteThread(LoadLibraryW)` injection. If you change anything about native DLL naming, versioning, or the manifest format, this is the file that also needs updating.

## Coding conventions

- Native C++: C++23, 4-space indent, `/W4 /WX /permissive-` (warnings fail the build) for everything except the vendored `OinksterImGui` target. Palette/theme constants for the ImGui overlay live as `IM_COL32` values in `OverlayUi.cpp`'s anonymous namespace (`Canvas`, `Surface`, `Pink`, etc.) — `Dx12Overlay.cpp`'s global `ImGuiStyle` setup duplicates the same values as floats since the two translation units don't currently share a header for them; keep them in sync if you change the palette.
- C#: four-space indentation, file-scoped namespaces, nullable reference types + implicit usings on, `PascalCase` for types/public members, `camelCase` for locals/parameters, XAML event handlers named by action (`SettingsButton_Click`).
- Generated `bin/`, `obj/`, and `Native/out/` are build output — never hand-edit or commit into them.
