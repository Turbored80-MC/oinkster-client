# Oinkster Native

This directory contains Oinkster's fair-play module runtime and DX12/ImGui workbench. The architecture follows the useful boundaries proven by open-source Bedrock clients: a centralized hook registry, an ordered module manager, a scoped cursor handler, and a ClickGUI that operates entirely through module metadata. Oinkster keeps its own implementation and visual identity.

The runtime never installs global keyboard or mouse hooks. While the workbench is open, input handling is limited to Minecraft-owned windows, the `ClipCursor` API used by the game, and Minecraft's own `GameInputRedist` mouse-state call. That scoped GameInput hook neutralizes movement, buttons, and wheel input only while the workbench owns the cursor, then rebases cumulative coordinates before returning control. The runtime does not contain packet mutation or game-memory write APIs.

## Prerequisites

- Visual Studio 2022 with **Desktop development with C++**
- MSVC x64 build tools and the Windows 10/11 SDK
- CMake 3.24 or newer

## Build and test

```powershell
cmake --preset windows-x64
cmake --build --preset debug
ctest --preset debug
```

The build produces `oinkster-client-x64.dll`, `Oinkster.ModuleHarness.exe`, and `Oinkster.Native.Tests.exe` under `Native/out/build/x64`.

## Runtime boundaries

`IBedrockAdapter` is the only boundary allowed to identify a game build or attach rendering/event sources. It must validate every required signature before changing process state, support exactly one manifest version, and fully detach on any failure. Modules receive typed read-only snapshots and cannot access raw game memory or mutable packets.

The workbench has module, HUD, and settings surfaces. The menu and clean-unload keys are configurable in both the launcher and the in-game Settings page.

## Module architecture

- `include/Oinkster/Module.hpp` - module contracts: identity, required capabilities, typed setting descriptors, HUD content (lines, graph, key grid), actions, and the integration mixins `IOverlayModule`, `IFrameBlendModule`, `IFramePacingModule`, `ISprintAssistModule`. Integration code (`Dx12Overlay`, `OverlayUi`) finds modules by these interfaces, never by id.
- `include/Oinkster/Events.hpp` - the only data that reaches modules: immutable, timestamped value snapshots (`PlayerTransform`, `EquipmentSnapshot`, `InventorySnapshot`, `EffectsSnapshot`, `NetworkMetrics`, `ServerState`, `ResourcePackState`, `CombatEvent`, `SoundEvent`, `ChatEvent`). No addresses or game objects.
- `src/ModuleCatalog.cpp` - the native catalog table (id, name, description, category, HUD flag, capabilities, factory). `Oinkster.Native.Tests` parses `Assets/module-catalog.json` and fails on any drift from this table, including `available` and `unavailableReason`.
- `src/Modules/*Modules.cpp` - the 36 module implementations, one file per category. Pure, unit-tested logic lives in `include/Oinkster/Modules/*Logic.hpp` and shared helpers in `FrameStatistics`, `RollingWindow` and `HudFormat`.
- `ModuleManager` - registration, lifecycle, dispatch, keybinds, capability-based availability, and settings normalization. No feature logic.

### Capabilities and availability

A module is available exactly when every capability it lists is Available. Adapters report capabilities with `ModuleManager::SetCapability`; every change is logged (`"event":"capability"`) with the capability id, state and reason. A module enabled by the player while unavailable is remembered and switches on automatically when its capabilities validate.

| Capability | Provided by | State for Minecraft 1.26.5203 |
|---|---|---|
| `render-metrics`, `frame-composition` | Oinkster's own DX12 Present hook | Available |
| `local-input`, `input-synthesis` | Oinkster's subclassed Minecraft window (posted key messages) | Available |
| `local-player-position` | `WorldTelemetry` (unique `InGamePlayScreen::_renderLevelPrep` signature) | Supported; Pending until the scan runs, then Available only on exactly one match |
| `local-player-rotation`, `effects`, `equipment`, `inventory` | `PlayerStateTelemetry` (entt `ActorRotationComponent` / `MobEffectsComponent` / `ActorEquipmentComponent` of the `_renderLevelPrep` Actor via `EnttRegistry`; the player inventory and ItemStacks via `GameItems`) | Supported; Pending until the first plausible read, then Available |
| `network-metrics`, `server-state`, `resource-packs`, `combat-events`, `selected-block-render`, `damage-render`, `sound-events`, `chat-events`, `camera-presentation`, `hand-presentation` | No adapter exists | Unavailable |

The supported build is defined once, in `include/Oinkster/SupportedBuild.hpp`. Signature scans go through `SignatureScan::FindUniqueInText`, which accepts a target only when exactly one location across all candidate patterns matches.

### Remaining live-validation work

These items are implemented and unit-tested against typed snapshots, but cannot be verified without the game. Nothing below is claimed as working in-game.

- **Game-memory adapters.** The remaining 12 roadmap modules need an unsupported capability (see the table above and the catalog). Their logic, settings and UI are complete, but no validated data source exists yet. Each needs a reviewed adapter that validates a unique signature and every pointer read (`SafeMemory`), then calls `SetCapability`. No offsets or signatures are invented for them.
- **`WorldTelemetry`.** 1.26.5203 is clang-compiled, so Flarial's 1.26.0-1.26.3 signatures no longer match (`Tools/SurveySignatures.py` finds 5 of 141). The hook (`InGamePlayScreen::_renderLevelPrep`) and offsets (`LevelRenderer+0x468` -> `LevelRendererPlayer+0x72C`) were re-derived from the build's own image and are documented in `WorldTelemetry.cpp`; the signature is verified unique statically, the offsets still need live confirmation. It reads the **camera** position and subtracts the 1.62-block eye height to get the player's feet; that is exact in first person while standing, but off while sneaking and in third person. Check `oinkster-native.log` for `world_telemetry_signature` and compare the `world_telemetry_sample` lines (which log both camera-vector candidates) with the in-game coordinates.
- **Sprint synthesis.** Toggle sprint posts an ordinary sprint-key press to the Minecraft window, the same path the menu uses to release held keys. Confirm in-game that Minecraft honors it with the player's sprint binding.
- **Motion blur.** The DX12 resource lifecycle is tested on WARP. Verify visually in-game with each backbuffer format the game uses (`IsBlendableFormat` refuses others).

## Metric definitions

- **FPS / frame time** - time-weighted over a trailing window: frames / summed frame time. Frame time is measured between Present calls.
- **FPS statistics** - over a trailing window (default 10 s, at most 8192 frames): average (time-weighted), minimum (1 / longest frame), and **1% low = 1 / mean of the slowest 1% of frame times**, reported only once the window holds at least 100 frames.
- **Input latency** - "input to frame": from the Minecraft window procedure receiving a key-down, button-down or wheel message to the start of the next Present call. Excludes device, OS, GPU and display latency. Samples over 1 s are discarded.
- **CPS** - left and right clicks in the trailing second, excluding clicks the ClickGUI consumed.
- **Movement speed** - path length / time over a trailing window. It resets instead of spiking on teleports (> 100 blocks/s), gaps (> 1 s) and dimension changes.
- **Session statistics** - counters reset when the module is enabled or Reset is pressed. They are never persisted.
- **Combo** - grows on confirmed hits on the same target within the timeout (default 2 s). A new target starts at 1. It resets on timeout, a reported miss, or damage taken. Clicks are never treated as hits.
- **Hit distance** - attacker eye to the nearest point of the target's bounding box.
- **Equipment warnings** - each slot warns once at or below the threshold, and re-arms only when the item changes or recovers above threshold + 5%.
- **Subtitles** - at most N captions. A repeated sound refreshes its caption (with a repeat count). When full, the caption nearest expiry is replaced.

## Module settings reference

Settings persist in `settings.json` under `legacyModuleSettings` as `"<module-id>.<key>": "<value>"`. Values are normalized on load. Out-of-range numbers are clamped. Malformed values, unknown choices, and reserved keys (Escape, Windows, mouse buttons, the menu/unload keys) fall back to the default. Unknown keys are kept for forward compatibility. Formats: bool `true`/`false`; numbers in the "C" locale; colors `#AARRGGBB`; keys as decimal Win32 virtual-key codes. Every HUD module also stores its position, scale, background, opacity and colors in `modules.<id>.hud`.

The in-game module browser stores favorites as `click-gui.favorite.<module-id>` boolean entries in the same map. These UI-owned keys are preserved by both the native runtime and launcher without changing the settings schema.

| Module | Key | Type / range | Default | Label |
|---|---|---|---|---|
| `fps` | `fps.smoothing-ms` | int 250..3000 | `1000` | Smoothing window |
| `fps` | `fps.show-label` | bool | `true` | Show label |
| `frame-time` | `frame-time.smoothing-ms` | int 100..3000 | `500` | Smoothing window |
| `frame-time` | `frame-time.decimals` | int 0..2 | `1` | Decimal places |
| `fps-statistics` | `fps-statistics.window-seconds` | int 2..60 | `10` | Statistics window |
| `fps-statistics` | `fps-statistics.show-frame-count` | bool | `false` | Show frame count |
| `frame-time-graph` | `frame-time-graph.samples` | int 60..600 | `240` | History length |
| `frame-time-graph` | `frame-time-graph.scale` | choice: auto / 16 / 33 / 50 / 100 | `auto` | Vertical scale |
| `frame-time-graph` | `frame-time-graph.show-current` | bool | `true` | Show current value |
| `process-memory` | - | HUD layout only | | |
| `performance-summary` | `performance-summary.window-seconds` | int 2..60 | `10` | 1% low window |
| `performance-summary` | `performance-summary.show-memory` | bool | `true` | Show memory |
| `cps` | `cps.show-right` | bool | `true` | Show right clicks |
| `cps` | `cps.style` | choice: labelled / compact | `labelled` | Style |
| `keystrokes` | `keystrokes.forward-key` | key: VK code 0-254, 0 = unbound | `87 (W)` | Forward key |
| `keystrokes` | `keystrokes.left-key` | key: VK code 0-254, 0 = unbound | `65 (A)` | Left key |
| `keystrokes` | `keystrokes.back-key` | key: VK code 0-254, 0 = unbound | `83 (S)` | Back key |
| `keystrokes` | `keystrokes.right-key` | key: VK code 0-254, 0 = unbound | `68 (D)` | Right key |
| `keystrokes` | `keystrokes.show-mouse` | bool | `true` | Show mouse buttons |
| `keystrokes` | `keystrokes.show-cps` | bool | `true` | Show CPS on mouse buttons |
| `keystrokes` | `keystrokes.show-jump` | bool | `true` | Show jump (Space) |
| `keystrokes` | `keystrokes.show-sneak-sprint` | bool | `false` | Show sneak and sprint (Shift / Ctrl) |
| `input-latency` | `input-latency.display` | choice: average / last / both | `average` | Show |
| `ping` | `ping.stale-seconds` | int 1..30 | `5` | Treat as stale after |
| `ping` | `ping.warn-ms` | int 20..1000 | `150` | Highlight above |
| `coordinates` | `coordinates.mode` | choice: block / exact | `block` | Show |
| `coordinates` | `coordinates.precision` | int 0..3 | `1` | Decimal places (exact position) |
| `coordinates` | `coordinates.layout` | choice: single / stacked | `single` | Layout |
| `coordinates` | `coordinates.stale-seconds` | int 1..30 | `2` | Treat as stale after |
| `direction` | `direction.intercardinal` | bool | `true` | Show intercardinal points (NE, SW ...) |
| `direction` | `direction.yaw-range` | choice: signed / unsigned | `signed` | Yaw range |
| `direction` | `direction.show-axis` | bool | `true` | Show facing axis |
| `movement-speed` | `movement-speed.unit` | choice: bps / kmh / mph | `bps` | Unit |
| `movement-speed` | `movement-speed.horizontal` | bool | `true` | Horizontal speed only |
| `movement-speed` | `movement-speed.window-ms` | int 100..2000 | `500` | Averaging window |
| `clock` | `clock.24-hour` | bool | `false` | 24-hour time |
| `clock` | `clock.show-seconds` | bool | `false` | Show seconds |
| `clock` | `clock.show-label` | bool | `true` | Show label |
| `stopwatch` | `stopwatch.show-milliseconds` | bool | `false` | Show milliseconds |
| `stopwatch` | `stopwatch.start-on-enable` | bool | `false` | Start automatically when enabled |
| `stopwatch` | `stopwatch.toggle-key` | key: VK code 0-254, 0 = unbound | `0 (unbound)` | Start / pause key |
| `session-statistics` | `session-statistics.show-input` | bool | `true` | Show key presses and clicks |
| `session-statistics` | `session-statistics.show-distance` | bool | `true` | Show distance travelled |
| `armor-status` | `armor-status.display` | choice: percent / remaining / both | `percent` | Durability as |
| `armor-status` | `armor-status.show-empty` | bool | `false` | Show empty slots |
| `armor-status` | `armor-status.show-names` | bool | `true` | Show item names |
| `armor-status` | `armor-status.warn-percent` | int 1..100 | `20` | Highlight at or below |
| `held-item-durability` | `held-item-durability.display` | choice: percent / remaining / both | `both` | Durability as |
| `held-item-durability` | `held-item-durability.include-offhand` | bool | `false` | Include off-hand item |
| `held-item-durability` | `held-item-durability.hide-undamageable` | bool | `true` | Hide items without durability |
| `held-item-durability` | `held-item-durability.warn-percent` | int 1..100 | `20` | Highlight at or below |
| `item-counter` | `item-counter.count` | choice: inventory / stack | `inventory` | Count |
| `item-counter` | `item-counter.show-name` | bool | `true` | Show item name |
| `potion-effects` | `potion-effects.max-shown` | int 1..12 | `6` | Effects shown |
| `potion-effects` | `potion-effects.show-level` | bool | `true` | Show level (II, III ...) |
| `potion-effects` | `potion-effects.sort` | choice: remaining / name | `remaining` | Sort by |
| `potion-effects` | `potion-effects.warn-seconds` | int 0..120 | `10` | Highlight below |
| `server-display` | `server-display.show-kind` | bool | `true` | Show session type |
| `server-display` | `server-display.show-addresses` | bool | `false` | Show address-like names |
| `resource-pack-display` | `resource-pack-display.max-shown` | int 1..6 | `1` | Packs shown |
| `resource-pack-display` | `resource-pack-display.show-version` | bool | `false` | Show pack version |
| `combo-counter` | `combo-counter.timeout-seconds` | float 0.5..10.0 | `2.0` | Combo timeout |
| `combo-counter` | `combo-counter.show-best` | bool | `false` | Show session best |
| `combo-counter` | `combo-counter.hide-when-zero` | bool | `false` | Hide when no combo |
| `hit-distance` | `hit-distance.decimals` | int 1..3 | `2` | Decimal places |
| `hit-distance` | `hit-distance.hold-seconds` | int 1..60 | `5` | Keep last value for |
| `equipment-warnings` | `equipment-warnings.threshold-percent` | int 1..90 | `15` | Warn at or below |
| `equipment-warnings` | `equipment-warnings.include-hands` | bool | `true` | Include held items |
| `equipment-warnings` | `equipment-warnings.alert-seconds` | int 0..15 | `3` | Highlight new warnings for |
| `equipment-warnings` | `equipment-warnings.hide-when-ok` | bool | `true` | Hide when nothing is low |
| `potion-counter` | `potion-counter.count-drinkable` | bool | `true` | Count drinkable potions |
| `potion-counter` | `potion-counter.count-splash` | bool | `true` | Count splash potions |
| `potion-counter` | `potion-counter.count-lingering` | bool | `false` | Count lingering potions |
| `potion-counter` | `potion-counter.breakdown` | bool | `false` | Show per-type breakdown |
| `custom-crosshair` | `custom-crosshair.size` | float 0.0..24.0 | `6.0` | Arm length |
| `custom-crosshair` | `custom-crosshair.gap` | float 0.0..16.0 | `3.0` | Gap |
| `custom-crosshair` | `custom-crosshair.thickness` | float 1.0..6.0 | `2.0` | Thickness |
| `custom-crosshair` | `custom-crosshair.color` | color | `#FFFFFFFF` | Color |
| `custom-crosshair` | `custom-crosshair.opacity` | float 0.1..1.0 | `1.0` | Opacity |
| `custom-crosshair` | `custom-crosshair.outline` | bool | `true` | Outline |
| `custom-crosshair` | `custom-crosshair.outline-thickness` | float 0.5..3.0 | `1.0` | Outline thickness |
| `custom-crosshair` | `custom-crosshair.center-dot` | bool | `false` | Center dot |
| `custom-crosshair` | `custom-crosshair.dot-size` | float 0.5..6.0 | `1.5` | Dot radius |
| `block-outline` | `block-outline.color` | color | `#FFF28FB5` | Outline color |
| `block-outline` | `block-outline.thickness` | float 1.0..5.0 | `2.0` | Thickness |
| `block-outline` | `block-outline.fill-opacity` | float 0.0..0.4 | `0.0` | Face fill opacity |
| `hurt-color` | `hurt-color.color` | color | `#FFFF3030` | Tint color |
| `hurt-color` | `hurt-color.opacity` | float 0.0..1.0 | `0.6` | Tint strength |
| `motion-blur` | `motion-blur.strength` | float 0.05..0.85 | `0.35` | Blend strength |
| `fov-profiles` | `fov-profiles.profile-1` | float 30..110 | `70.0` | Profile 1 FOV |
| `fov-profiles` | `fov-profiles.profile-2` | float 30..110 | `90.0` | Profile 2 FOV |
| `fov-profiles` | `fov-profiles.profile-3` | float 30..110 | `100.0` | Profile 3 FOV |
| `fov-profiles` | `fov-profiles.active` | int 1..3 | `1` | Active profile |
| `fov-profiles` | `fov-profiles.cycle-key` | key: VK code 0-254, 0 = unbound | `0 (unbound)` | Cycle profile key |
| `view-bobbing` | `view-bobbing.strength` | float 0.0..1.0 | `0.25` | Bobbing strength |
| `hand-view` | `hand-view.offset-x` | float -0.5..0.5 | `0.0` | Horizontal offset |
| `hand-view` | `hand-view.offset-y` | float -0.5..0.5 | `0.0` | Vertical offset |
| `hand-view` | `hand-view.offset-z` | float -0.5..0.5 | `0.0` | Depth offset |
| `hand-view` | `hand-view.scale` | float 0.5..1.5 | `1.0` | Scale |
| `subtitles` | `subtitles.lifetime-seconds` | float 1.0..10.0 | `3.0` | Caption lifetime |
| `subtitles` | `subtitles.max-lines` | int 1..12 | `6` | Maximum captions |
| `subtitles` | `subtitles.max-distance` | float 4.0..64.0 | `16.0` | Ignore sounds beyond |
| `subtitles` | `subtitles.show-distance` | bool | `true` | Show distance |
| `chat-tools` | `chat-tools.timestamps` | bool | `true` | Timestamps |
| `chat-tools` | `chat-tools.24-hour` | bool | `true` | 24-hour timestamps |
| `chat-tools` | `chat-tools.strip-formatting` | bool | `true` | Strip color/format codes |
| `chat-tools` | `chat-tools.hide-nicknames` | bool | `false` | Hide nicknames |
| `chat-tools` | `chat-tools.hidden-names` | text (<= 64) | `""` | Names to hide (comma separated) |
| `chat-tools` | `chat-tools.replacement` | text (<= 64) | `"Player"` | Replacement |
| `chat-tools` | `chat-tools.lines` | int 1..20 | `8` | Lines shown |
| `toggle-sprint-dynamic-fps` | `toggle-sprint-dynamic-fps.sprint-enabled` | bool | `true` | Toggle sprint |
| `toggle-sprint-dynamic-fps` | `toggle-sprint-dynamic-fps.sprint-mode` | choice: always / toggle | `always` | Sprint mode |
| `toggle-sprint-dynamic-fps` | `toggle-sprint-dynamic-fps.sprint-toggle-key` | key: VK code 0-254, 0 = unbound | `0 (unbound)` | Sprint toggle key |
| `toggle-sprint-dynamic-fps` | `toggle-sprint-dynamic-fps.sprint-key` | key: VK code 0-254, 0 = unbound | `17 (Ctrl)` | Game sprint key |
| `toggle-sprint-dynamic-fps` | `toggle-sprint-dynamic-fps.forward-key` | key: VK code 0-254, 0 = unbound | `87 (W)` | Game forward key |
| `toggle-sprint-dynamic-fps` | `toggle-sprint-dynamic-fps.sneak-key` | key: VK code 0-254, 0 = unbound | `16 (Shift)` | Game sneak key |
| `toggle-sprint-dynamic-fps` | `toggle-sprint-dynamic-fps.dynamic-fps-enabled` | bool | `true` | Dynamic FPS |
| `toggle-sprint-dynamic-fps` | `toggle-sprint-dynamic-fps.background-fps` | int 1..60 | `15` | Background frame cap |
| `toggle-sprint-dynamic-fps` | `toggle-sprint-dynamic-fps.minimized-fps` | int 1..30 | `5` | Minimized frame cap |
