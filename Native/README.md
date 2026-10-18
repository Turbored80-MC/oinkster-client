# Oinkster Native

This directory contains Oinkster's fair-play module runtime and DX12/ImGui workbench. The architecture follows the useful boundaries proven by open-source Bedrock clients: a centralized hook registry, an ordered module manager, a scoped cursor handler, and a ClickGUI that operates entirely through module metadata. Oinkster keeps its own implementation and visual identity.

The runtime never installs global keyboard or mouse hooks. While the workbench is open, input handling is limited to Minecraft-owned windows, the `ClipCursor` API used by the game, and Minecraft's own `GameInputRedist` mouse-state call. That scoped GameInput hook neutralizes movement, buttons, and wheel input only while the workbench owns the cursor, then rebases cumulative coordinates before returning control. Packet sending is restricted to explicitly enabled social actions and validated, build-specific packet layouts learned from the game's own traffic; malformed, stale, cross-connection, and over-limit requests fail closed.

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

`IBedrockAdapter` is the only boundary allowed to identify a game build or attach rendering/event sources. It must validate every required signature before changing process state, support exactly one manifest version, and fully detach on any failure. Modules receive typed read-only snapshots and cannot access raw game memory or mutable packets. The one exception to "adapters only read" is `PresentationAdapter`, which changes local render values only (a camera FOV it restores within the same call, and the return values of the gamma, view-bobbing and time-of-day getters); it never touches packets, world state or physics.

The workbench has module, HUD, and settings surfaces. The menu and clean-unload keys are configurable in both the launcher and the in-game Settings page.

## Module architecture

- `include/Oinkster/Module.hpp` - module contracts: identity, required capabilities, typed setting descriptors, movable HUD content (lines, graph, key grid), the anchored `IDebugOverlayModule` left/right row contract, actions, and the integration mixins `IOverlayModule`, `IFrameBlendModule`, `IFramePacingModule`, `ISprintAssistModule`, `IMouseShapingModule` (wheel block and look sensitivity, applied by `MouseInputBlocker`), `IKeyTransformModule` (Null Movement, applied on the window thread), `IHeldKeyRestoreModule` (re-presses held keys when a game screen closes) and `IPostProcessModule` (colour grading through `PostProcessPass`). Integration code (`Dx12Overlay`, `OverlayUi`) finds modules by these interfaces, never by id.
- `include/Oinkster/Events.hpp` - the only data that reaches modules: immutable, timestamped value snapshots (`PlayerTransform`, `EquipmentSnapshot`, `InventorySnapshot`, `EffectsSnapshot`, `NetworkMetrics`, `ServerState`, `ResourcePackState`, `CombatEvent`, `SoundEvent`, `ChatEvent`). No addresses or game objects.
- `src/ModuleCatalog.cpp` - the native catalog table (id, name, description, category, HUD flag, capabilities, factory). `Oinkster.Native.Tests` parses `Assets/module-catalog.json` and fails on any drift from this table, including `available` and `unavailableReason`.
- `src/Modules/*Modules.cpp` - the 57 module implementations, one file per category. Pure, unit-tested logic lives in `include/Oinkster/Modules/*Logic.hpp` and shared helpers in `FrameStatistics`, `RollingWindow` and `HudFormat`.
- `ModuleManager` - registration, lifecycle, dispatch, keybinds, capability-based availability, and settings normalization. No feature logic.

### Capabilities and availability

A module is available exactly when every capability it lists is Available. Adapters report capabilities with `ModuleManager::SetCapability`; every change is logged (`"event":"capability"`) with the capability id, state and reason. A module enabled by the player while unavailable is remembered and switches on automatically when its capabilities validate.

| Capability | Provided by | State for Minecraft 1.26.5203 |
|---|---|---|
| `render-metrics`, `frame-composition` | Oinkster's own DX12 Present hook | Available |
| `local-input`, `input-synthesis` | Oinkster's subclassed Minecraft window (posted key messages) | Available |
| `local-player-position` | `WorldTelemetry` (unique `InGamePlayScreen::_renderLevelPrep` signature) | Supported; Pending until the scan runs, then Available only on exactly one match |
| `local-player-rotation`, `effects`, `equipment`, `inventory` | `PlayerStateTelemetry` (entt `ActorRotationComponent` / `MobEffectsComponent` / `ActorEquipmentComponent` of the `_renderLevelPrep` Actor via `EnttRegistry`; the player inventory and ItemStacks via `GameItems`) | Supported; Pending until the first plausible read, then Available |
| `network-metrics`, `server-state` | `SessionTelemetry` (unique `RakNet::RakPeer::Receive` signature; the connected remote's ping ring, and `LevelData::mLevelName` via the `_renderLevelPrep` Actor's Level) | Supported; Pending until the first plausible read, then Available |
| `combat-events` | `CombatTelemetry` (entt `MobHurtTimeComponent` of the local player and of the entity a click was aimed at, `AABBShapeComponent` for its bounds) | Supported; Pending until the first plausible read, then Available |
| `sound-events` | `SoundTelemetry` (unique signature of the LevelRenderer world-sound helper every LevelListener sound goes through) | Supported; Pending until the first sound, then Available |
| `resource-packs` | `ResourcePackTelemetry` (the ResourcePackManager the `_renderLevelPrep` Actor's ILevel returns: level stack, then global stack) | Supported; Pending until the first plausible read, then Available |
| `chat-events` | `ChatTelemetry` (unique signature of the client chat-line sink every TextPacket type handler calls) | Supported; Pending until the first line, then Available |
| `packet-capture` | `ReplayPacketBridge` (validated decoded server-to-client packet boundary) | Supported; Pending until a server connection is classified, then Available |
| `packet-send` | `PacketSender` (validated `BatchedNetworkPeer::sendPacket`; command/form layouts learned per build) | Supported; Pending until the current server peer validates, then Available |
| `camera-presentation`, `camera-rotation`, `camera-motion`, `third-person-presentation`, `lighting-presentation`, `time-presentation`, `weather-presentation`, `screen-effects-presentation`, `hand-presentation`, `damage-render`, `selected-block-render`, `perspective-presentation` | `PresentationAdapter` (see Remaining live-validation work) | Pending until each hook validates live |

The supported build is defined once, in `include/Oinkster/SupportedBuild.hpp`. Signature scans go through `SignatureScan::FindUniqueInText`, which accepts a target only when exactly one location across all candidate patterns matches.

### Replay status

Goal: Java Replay Mod / Flashback-style replays: record a world session, then load it back **inside Minecraft** and watch it with a free camera. Capture, playback, timeline seeking, camera paths, and recorded-player body reconstruction are implemented and unit-tested; the remaining limits are called out below as live-validation work rather than described as missing code.

- **Capture** (`ReplayPacketBridge.cpp`, Capability::PacketCapture): hooks `NetworkConnection::receivePacket` (RVA `0xD935A0`, unique signature). It is the only callee the game's decode loop (RVA `0x16AADF0`) uses to pull the next packet: on a 0 return the `std::string` it filled holds exactly one packet after decryption and decompression, header varint included (the loop builds a `ReadOnlyBinaryStream` over it at offset 0 and passes `header & 0x3FF` to `MinecraftPackets::createPacket`). Each connection's direction is decided from the ids it carries (`ReplayCapture.hpp`: server-only ids such as NetworkSettings/StartGame/LevelChunk vs client-only ids such as Login/PlayerAuthInput). Only the first five bytes of a packet are read until its connection is known to be server->client, so the singleplayer server's view of the client (Login included) is never copied. Packets are published as `ReplayPacketEvent` only while Replay Mod is enabled. Log: `replay_capture_signature`, `replay_capture_validated`, `replay_capture_session` (session number, whether the join was seen from its start) and `replay_capture_sample` (first 24 ids and sizes per session, never contents).
- **Recording** (`ReplayModModule`, `Modules/StatusModules.cpp`): while Replay Mod is on, every world joined is captured from its first packet into a hidden session file (`replays/.session/`, deleted on leave and on enable), with camera samples every 50 ms. Start/Finish cut a **clip**: the published replay is the session up to Finish plus a `clip-start` marker and the clip's own `pause`/`resume`/`mark N` markers, so a clip started mid-session still carries the world. Playback removes paused wall-clock spans, omits paused camera motion, and applies packets received during a pause as an instantaneous state catch-up when the clip resumes. Any number of clips per session; `auto-record` (default off) starts one on join. A running clip is saved on Disconnect, on the next join, or when no level has rendered for 15 s (`replay_auto_finish`). A world joined before Oinkster loaded cannot be recorded. Saves log `replay_saved`. Packets are stored uncompressed (tens of MB per session).
- **Playback** (`ReplayInjector.cpp`, `ReplayPlayback.cpp`, `ReplayPacketBridge.cpp`): in-game, with no network server (the approach of wo55555/Playback). Watch loads the replay on a worker thread, then waits for the viewer to open any singleplayer world; connections to external servers are never taken over (screen telemetry must have announced a local-world load). The game's integrated server completes the real handshake up to and including its ResourcePackStack; from the next packet on, every server packet on that connection is dropped at the `NetworkConnection::receivePacket` hook and the recording is returned in its place, one packet per call: everything before the clip start at once (at most 256 packets per poll), the clip in real time (pause and 0.5-4x speed from the Esc-menu panel). A recorded packet is handed over by pointing the game's receive string at our own buffer for one decode and restoring the game's string at the start of the next call, so the game never frees memory it did not allocate; unload waits for that and blocks if it does not happen. StartGame's player game mode is patched to spectator; packets that would move or control the viewer (MovePlayer/SetActorMotion/Respawn for the recorded player's runtime id, UpdateAbilities for its unique id, game-type changes, movement corrections) and Disconnect/Transfer are dropped. Joining another world ends the replay. Capture is suppressed while a replay is armed or playing. Use a throwaway world: the local server still simulates the viewer in it. Log: `replay_inject_ready`, `replay_inject_load`, `replay_inject_phase`, `replay_inject_skip`.
- **Pause-screen panel** (`Dx12Overlay.cpp`, `UpdateMinecraftPauseVisible`): Esc during gameplay (cursor hidden on the previous frame) arms a 1.5 s window; the cursor appearing inside it, while a level is rendering, latches the panel. It releases when the cursor stays hidden for 150 ms, the level stops rendering for 250 ms (quitting the world), focus is lost, or the ClickGUI opens. Logged as `replay_pause_detect`.
- **Replays menu** (`OverlayUi.cpp`): the Minecraft-style **Replays** button in the top-left of the title screen only. `ScreenTelemetry` hooks the game's own title-screen listener (RVA `0x87F1F0`, screen-event name at +0x30, flag at +0x98; `screen_event` logs names) and the button also requires that no level has rendered for 400 ms (`WorldTelemetry::LevelRenderedWithin`, `world_presence`); without screen events it falls back to "outside a world". The in-world ClickGUI REPLAYS tile lists replays but cannot Watch. Outside a world the menu key does nothing. Intro animation while files are summarized one per frame, then a library of cards (path thumbnail, date, length, marks, size, packet count; COMPLETE / RECOVERABLE / NO WORLD DATA). Clicking a card watches it in the world; Preview opens the top-down path view with the capture's packet count and first ids.
- **Format**: `ReplayFile` implements format 1.0 of the append-only `.oinkreplay` container (timestamp, kind, bounded length and checksum per record; `.partial` sibling while recording; an End record marks a clean save). Packet records are `IncomingPacket` with the raw packet bytes.

### Social and Java-style overlays

- **Player list** reads bounded PlayerList packets, filters entries without a real XUID when requested, sorts by cleaned player name or server order, lays rows out in Java-style columns, and uses a neutral head fallback if a classic skin face cannot be decoded. A new packet session, disconnect, disable, or non-gameplay screen clears or hides the relevant state.
- **Hive utils** runs only while `ServerState` identifies The Hive. Re-queue, invite acceptance, map voting, `/connection` detection, and join-code copying each require their corresponding user setting. Commands and form replies are sent only after the current server peer validates and Oinkster has learned that packet layout from the player's own command or form response.
- **Skin grabber** saves sanitized PNG filenames under `%LOCALAPPDATA%\Oinkster\Client\skins`, reports skin and cape write failures separately, preserves the first complete self-skin for restoration, and refuses wear operations until the current server connection validates.
- **Debug menu** is enabled but hidden by default. F3 (configurable with `debug-menu.key`) toggles it only during gameplay; repeats, ClickGUI-captured input, and menus are ignored. It uses compact white text on individual square charcoal backplates, anchors independent groups to both safe screen edges, and omits unavailable or stale telemetry. It is intentionally not part of the movable HUD editor.
- **Chunk borders** use Java F3+G hierarchy: faint neighboring columns and minor wall grids, emphasized 16-block section lines, stronger current boundaries, and current-chunk corner pillars. Projection clips deterministically at the near plane and screen edge, and opacity falls with distance.

Analysis helpers live in `Tools/FindStringXrefs.py` and `Tools/FindCallXrefs.py`; both report RVAs suitable for `dumpbin /disasm /range` against the memory image under `%LOCALAPPDATA%\Oinkster\Client\analysis`. `Tools/SurveySignatures.py --pattern NAME=HEX` checks a pattern is unique.

### Remaining live-validation work

These items are implemented and unit-tested against typed snapshots, but cannot be verified without the game. Nothing below is claimed as working in-game.

- **`PresentationAdapter`.** The only adapter that changes values the game renders with; always local (never packets, world state or physics), and every override is undone in the same frame or on disable. Every target is a unique signature (`presentation_signature`, eight lines). Each capability stays Pending until its first plausible read; the `presentation_*_validated` log lines name what was seen.
  - *Camera* (FOV profiles, zoom, freelook, cinematic camera, hurt camera, third-person distance): the LevelRendererPlayer camera update (RVA `0x46CFF30`) and the active-camera lookup it calls (RVA `0x9ED0A0`, returning `{EntityRegistry*, basic_registry*, EntityId, valid}`). For that update only, the active `MinecraftCamera::CameraComponent`'s FOV (+0x50, radians), camera-to-world rotation quaternion (+0x30) and position (+0x40, copied into LevelRendererPlayer+0x654 for the frame) are overridden, then restored. Hurt camera scales the camera roll (0 removes the hurt tilt; it also levels any other roll). Cinematic camera renders a smoothed rotation that trails your aim. Freelook captures mouse movement (`MouseInputBlocker::SetLookCapture`: the game sees a frozen pointer, clicks still pass) and eases back on release. In third person the camera orbits the point on the vanilla view ray nearest your eye (`RenderPositionComponent` + an eye offset learned in first person) at the third-person distance; the game's own wall collision is not applied to a longer distance. Logs: `presentation_camera_validated`, `presentation_transform_validated` (and whether mouse capture is ready), `presentation_pivot_validated`; `presentation_boom_probe` logs `CameraThirdPersonBoomComponent` (hash `0xEF14FB9A`).
  - *View bobbing*: the Options getter for BoolOption `0x26` (RVA `0xFB5DF0`) answers false at strength 0; partial strengths are not implemented.
  - *Fullbright*: `Options::getGamma` (RVA `0xFB7880`, FloatOption `0x32`) returns the brightness setting.
  - *Time changer*: the sky time-of-day function (RVA `0x2312080`) re-evaluated with the frozen tick count, render thread only, so a local world's integrated server keeps real time.
  - *Weather*: `Weather::tick` (RVA `0x32AAD60`; rain at +0x34/+0x38/+0x3C previous/current/target, lightning at +0x40/+0x44/+0x48, targets set by `Weather::levelEvent` at RVA `0x32AAD00`). The game's own levels are restored before each tick and the scaled ones written after, so weather progresses normally. Rain and snow share one level; the snow slider follows rain.
  - *Screen overlays*: only the vignette (`vignette_renderer`, render at RVA `0x8DD5C00`) is hooked; 0% skips it. Fire, powder snow, portal, pumpkin and nausea overlays have no code-referenced renderer in this build and are unchanged.
  - *Hand view*: `ItemInHandRenderer::renderItem` (RVA `0x56095C0`). For the local player in first person, the camera world matrix stack's top (`[[context+0x28]+0x18]+0x40`, an MSVC deque of 64-byte matrices) is multiplied by the offset and scale before the item pushes onto it, and restored after.
  - *Hurt color*: every Actor keeps its damage tint at +0x2B0 (constructors store `{1, 0, 0, 0.6}`). Actors come from the client registry's `ActorOwnerComponent` pool, checked against their own EntityContext, 24 per frame; disabling or unloading writes the vanilla tint back.
  - *Block outline*: the local player's `ILevel` (Actor+0x1D8) slot `0xA78` returns the block hit result (0x88-byte HitResult: type +0x18, face +0x1C, BlockPos +0x20); the getter must be a plain field access. The outline is drawn by Oinkster's overlay from this frame's camera (only edges of camera-facing faces, plus the looked-at face filled), on top of the game's own outline, and is a full cube for every block shape. Logs: `presentation_selection_validated`, or `presentation_hit_result_unvalidated` with the getter's bytes.
- **Sprint synthesis.** Toggle sprint posts an ordinary sprint-key press to the Minecraft window, the same path the menu uses to release held keys. Confirm in-game that Minecraft honors it with the player's sprint binding.
- **`CameraTelemetry` (crosshair perspective).** The perspective comes from the game's own option (`Options::getPlayerViewPerspective`: 0 first person, 1 third person back, 2 third person front), read through plain field loads from the `_renderLevelPrep` Actor: LocalPlayer+0xD70 is the ClientInstance, whose `getOptions` (vtable slot 0x578) is checked to be `mov rax, [rcx+disp]; ret` and supplies the Options offset (+0xE10); the Options perspective getter (slot 0x3C0) is checked byte for byte before the option at +0x10 + 3 * 8 is trusted, and that option must carry id 3 and the telemetry name `playerViewPerspective`. Overrides (OptionImpl+0x238) are followed the way the getter does. Any mismatch publishes `Unknown`, and the adapter publishes `Unknown` before it detaches. Check the log for `camera_validated` and one `camera_perspective` line per F5 press. Not covered: a server forcing a camera preset (`/camera`) without changing the option.
- **`CrosshairTexture` (game crosshair in third person).** With `custom-crosshair.source = game` and `third-person` on, Oinkster redraws the game's crosshair only while `CameraTelemetry` reports a third-person camera, since Bedrock hides it there; first person and `Unknown` draw nothing, so the game's own crosshair is never doubled. The texture is `textures/ui/cross_hair.png` from the first active resource pack that has a readable one (pack uuids come from PackManifest+0x150/+0x158 and are matched to `manifest.json` in the resource_packs, development_resource_packs and world pack folders and the game's built-in packs; a loose PNG or the pack's `__brarchive/textures/ui.brarchive`), else vanilla (`vanilla_base`). Encrypted packs (Marketplace, most server packs) cannot be read and fall through to the next pack. Files are read on a worker thread. Check `crosshair_texture` (size and which pack won). Pack priority order is the same assumption as `ResourcePackTelemetry` (highest first). Oinkster never hides or patches the game's own crosshair, so `custom` and `drawn` draw on top of it.
- **Motion blur.** The DX12 resource lifecycle is tested on WARP. Verify visually in-game with each backbuffer format the game uses (`IsBlendableFormat` refuses others).

### Adapter research notes (1.26.5203, not yet implemented)

RVAs are into the dumped image; nothing here ships until it is wired, tested and confirmed live.

- **Camera FOV / view bobbing.** There is no `getFov` on this build. The world camera is the entt `MinecraftCamera::CameraComponent` (0x120 bytes; FOV in radians at `+0x50`, aspect `+0x4C`, near/far `+0x54/+0x58`), read by the LevelRendererPlayer camera update at RVA `0x46CFF30`, which builds the projection inline and through RVA `0x9F4EB0`. The FOV option is `FloatOption` id `0x2F` (range 30-110) with an inlined getter, so nothing returns the FOV for a hook to substitute. Bobbing is `VanillaCamera::CameraBobComponent` (systems at RVA `0x6761C00`/`0x6762200`). Applying either needs a write to a camera component the renderer reads.
- **Block outline, hurt color, hand view.** Leads: the `selection_overlay*` materials (outline), the entity `overlay_color` uniform (hurt tint), and the `ItemInHand*` frame-builder passes (hand). Like FOV, each would change render state the game reads rather than return a value.

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

Settings persist in `settings.json` under `legacyModuleSettings` as `"<module-id>.<key>": "<value>"`. Values are normalized on load. Out-of-range numbers are clamped. Malformed values, unknown choices, and reserved keys (Escape, Windows, mouse buttons, the menu/unload keys) fall back to the default. Unknown keys are kept for forward compatibility. Formats: bool `true`/`false`; numbers in the "C" locale; colors `#AARRGGBB`; keys as decimal Win32 virtual-key codes. Every movable HUD module also stores its position, scale, background, opacity and colors in `modules.<id>.hud`.

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
| `replay-mod` | `replay-mod.auto-record` | bool | `false` | Start recording when you join a world |
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
| `custom-crosshair` | `custom-crosshair.source` | choice: custom / drawn / game | `custom` | Crosshair source |
| `custom-crosshair` | `custom-crosshair.third-person` | bool | `true` | Show in 3rd person (game source: redraws the pack/vanilla crosshair in third person) |
| `custom-crosshair` | `custom-crosshair.game-scale` | float 1.0..8.0 | `2.0` | Size (game source) |
| `custom-crosshair` | `custom-crosshair.perspective` | choice: first-person / all | `first-person` | Show custom crosshair (custom and drawn) |
| `custom-crosshair` | `custom-crosshair.pixels` | pixels: 16x16, 64 hex digits | a 10x2 plus | Draw (drawn source; edited in the ClickGUI) |
| `custom-crosshair` | `custom-crosshair.pixel-scale` | float 1.0..8.0 | `2.0` | Pixel size (drawn source) |
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
| `view-bobbing` | `view-bobbing.strength` | float 0.0..1.0 | `0.0` | Bobbing strength (only 0 has an effect so far) |
| `hand-view` | `hand-view.offset-x` | float -0.5..0.5 | `0.0` | Horizontal offset |
| `hand-view` | `hand-view.offset-y` | float -0.5..0.5 | `0.0` | Vertical offset |
| `hand-view` | `hand-view.offset-z` | float -0.5..0.5 | `0.0` | Depth offset |
| `hand-view` | `hand-view.scale` | float 0.5..1.5 | `1.0` | Scale |
| `zoom` | `zoom.fov` | float 5..70 | `30.0` | Zoom FOV |
| `zoom` | `zoom.mode` | choice: hold / toggle | `hold` | Activation |
| `zoom` | `zoom.key` | key: VK code 0-254, 0 = unbound | `67 (C)` | Activation key |
| `zoom` | `zoom.transition-ms` | int 0..500 | `120` | Transition |
| `freelook` | `freelook.mode` | choice: hold / toggle | `hold` | Activation |
| `freelook` | `freelook.key` | key: VK code 0-254, 0 = unbound | `18 (Alt)` | Activation key |
| `freelook` | `freelook.sensitivity` | float 0.1..3.0 | `1.0` | Sensitivity |
| `freelook` | `freelook.return-ms` | int 0..500 | `150` | Return transition |
| `cinematic-camera` | `cinematic-camera.strength` | float 0.0..1.0 | `0.75` | Smoothing |
| `cinematic-camera` | `cinematic-camera.transition-ms` | int 0..500 | `120` | Transition |
| `third-person-distance` | `third-person-distance.distance` | float 1.0..20.0 | `4.0` | Camera distance |
| `third-person-distance` | `third-person-distance.rear` | bool | `true` | Rear camera |
| `third-person-distance` | `third-person-distance.front` | bool | `true` | Front camera |
| `fullbright` | `fullbright.multiplier` | float 1.0..10.0 | `5.0` | Brightness |
| `time-changer` | `time-changer.ticks` | int 0..23999 | `6000` | Frozen time |
| `weather-suppression` | `weather-suppression.rain` | float 0..100 | `0.0` | Rain visibility (%) |
| `weather-suppression` | `weather-suppression.snow` | float 0..100 | `0.0` | Snow visibility (%) |
| `weather-suppression` | `weather-suppression.thunder` | float 0..100 | `0.0` | Thunder intensity (%) |
| `screen-overlay-suppression` | `screen-overlay-suppression.fire-opacity` | float 0..100 | `0.0` | Fire opacity (%) |
| `screen-overlay-suppression` | `screen-overlay-suppression.powder-snow-opacity` | float 0..100 | `0.0` | Powder snow opacity (%) |
| `screen-overlay-suppression` | `screen-overlay-suppression.portal-opacity` | float 0..100 | `0.0` | Portal opacity (%) |
| `screen-overlay-suppression` | `screen-overlay-suppression.pumpkin-opacity` | float 0..100 | `0.0` | Pumpkin opacity (%) |
| `screen-overlay-suppression` | `screen-overlay-suppression.vignette-opacity` | float 0..100 | `0.0` | Vignette opacity (%) |
| `screen-overlay-suppression` | `screen-overlay-suppression.nausea-opacity` | float 0..100 | `0.0` | Nausea opacity (%) |
| `hurt-camera` | `hurt-camera.strength` | float 0..100 | `0.0` | Camera shake (%) |
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
| `toggle-sneak` | `toggle-sneak.toggle-key` | key: VK code 0-254, 0 = unbound | `88 (X)` | Toggle key |
| `toggle-sneak` | `toggle-sneak.sneak-key` | key: VK code 0-254, 0 = unbound | `16 (Shift)` | Game sneak key |
| `scroll-wheel-disabler` | `scroll-wheel-disabler.bypass-key` | key: VK code 0-254, 0 = unbound | `0 (unbound)` | Scroll while held |
| `sensitivity-multiplier` | `sensitivity-multiplier.horizontal` | float 0.1..3.0 | `1.0` | Horizontal |
| `sensitivity-multiplier` | `sensitivity-multiplier.vertical` | float 0.1..3.0 | `1.0` | Vertical |
| `sensitivity-multiplier` | `sensitivity-multiplier.hold-key` | key: VK code 0-254, 0 = unbound | `0 (unbound)` | Apply only while held |
| `null-movement` | `null-movement.horizontal` | bool | `true` | Left / right |
| `null-movement` | `null-movement.vertical` | bool | `false` | Forward / back |
| `null-movement` | `null-movement.left-key` | key: VK code 0-254, 0 = unbound | `65 (A)` | Game left key |
| `null-movement` | `null-movement.right-key` | key: VK code 0-254, 0 = unbound | `68 (D)` | Game right key |
| `null-movement` | `null-movement.forward-key` | key: VK code 0-254, 0 = unbound | `87 (W)` | Game forward key |
| `null-movement` | `null-movement.back-key` | key: VK code 0-254, 0 = unbound | `83 (S)` | Game back key |
| `modern-keyboard-handling` | `modern-keyboard-handling.forward-key` | key: VK code 0-254, 0 = unbound | `87 (W)` | Game forward key |
| `modern-keyboard-handling` | `modern-keyboard-handling.back-key` | key: VK code 0-254, 0 = unbound | `83 (S)` | Game back key |
| `modern-keyboard-handling` | `modern-keyboard-handling.left-key` | key: VK code 0-254, 0 = unbound | `65 (A)` | Game left key |
| `modern-keyboard-handling` | `modern-keyboard-handling.right-key` | key: VK code 0-254, 0 = unbound | `68 (D)` | Game right key |
| `modern-keyboard-handling` | `modern-keyboard-handling.jump-key` | key: VK code 0-254, 0 = unbound | `32 (Space)` | Game jump key |
| `modern-keyboard-handling` | `modern-keyboard-handling.sneak-key` | key: VK code 0-254, 0 = unbound | `16 (Shift)` | Game sneak key |
| `modern-keyboard-handling` | `modern-keyboard-handling.sprint-key` | key: VK code 0-254, 0 = unbound | `17 (Ctrl)` | Game sprint key |
| `snap-look` | `snap-look.view` | choice: front / back | `front` | Look |
| `snap-look` | `snap-look.mode` | choice: hold / toggle | `hold` | Activation |
| `snap-look` | `snap-look.key` | key: VK code 0-254, 0 = unbound | `86 (V)` | Activation key |
| `chunk-borders` | `chunk-borders.chunk-color` | color | `#FFFFD54A` | Wall grid |
| `chunk-borders` | `chunk-borders.corner-color` | color | `#FFFF4A4A` | Chunk corners |
| `chunk-borders` | `chunk-borders.neighbor-color` | color | `#FF4AA8FF` | Neighbour corners |
| `chunk-borders` | `chunk-borders.thickness` | float 0.5..4.0 | `1.5` | Line thickness |
| `chunk-borders` | `chunk-borders.grid` | int 0..8 | `2` | Grid spacing |
| `chunk-borders` | `chunk-borders.range` | int 4..96 | `24` | Vertical range |
| `post-processing` | `post-processing.saturation` | float 0.0..2.0 | `1.2` | Saturation |
| `post-processing` | `post-processing.contrast` | float 0.5..1.5 | `1.05` | Contrast |
| `post-processing` | `post-processing.brightness` | float -0.3..0.3 | `0.0` | Brightness |
| `post-processing` | `post-processing.sharpen` | float 0.0..1.0 | `0.0` | Sharpen |
| `post-processing` | `post-processing.vignette` | float 0.0..1.0 | `0.0` | Vignette |
| `post-processing` | `post-processing.tint` | color | `#FFFFFFFF` | Tint |
| `post-processing` | `post-processing.tint-strength` | float 0.0..1.0 | `0.0` | Tint strength |
| `debug-menu` | `debug-menu.performance` | bool | `true` | Performance |
| `debug-menu` | `debug-menu.network` | bool | `true` | Connection |
| `debug-menu` | `debug-menu.target` | bool | `true` | Targeted block |
| `debug-menu` | `debug-menu.decimals` | int 0..5 | `3` | Coordinate decimals |
| `debug-menu` | `debug-menu.key` | key: VK code 0-254, 0 = unbound | `114 (F3)` | Show key |
| `debug-menu` | `debug-menu.scale` | float 0.6..2.0 | `1.0` | Scale |
| `player-list` | `player-list.key` | key: VK code 0-254, 0 = unbound | `9 (Tab)` | Show key |
| `player-list` | `player-list.mode` | choice: hold / toggle | `hold` | Activation |
| `player-list` | `player-list.heads` | bool | `true` | Player heads |
| `player-list` | `player-list.hide-npcs` | bool | `true` | Hide server NPCs |
| `player-list` | `player-list.sort` | choice: name / server | `name` | Order |
| `player-list` | `player-list.columns` | int 0..6 | `0` | Columns; 0 is automatic |
| `player-list` | `player-list.scale` | float 0.6..2.0 | `1.0` | Scale |
| `hive-utils` | `hive-utils.requeue` | bool | `true` | Auto re-queue |
| `hive-utils` | `hive-utils.requeue-death` | bool | `false` | Re-queue on death |
| `hive-utils` | `hive-utils.requeue-eliminated` | bool | `true` | Re-queue when your team is eliminated |
| `hive-utils` | `hive-utils.hub-instead` | bool | `false` | Go to the hub instead |
| `hive-utils` | `hive-utils.delay-ms` | int 0..5000 | `500` | Re-queue delay in milliseconds |
| `hive-utils` | `hive-utils.requeue-key` | key: VK code 0-254, 0 = unbound | `0 (unbound)` | Re-queue now key |
| `hive-utils` | `hive-utils.detect-game` | bool | `true` | Detect the current game |
| `hive-utils` | `hive-utils.party-accept` | bool | `false` | Accept party invites |
| `hive-utils` | `hive-utils.friend-accept` | bool | `false` | Accept friend invites |
| `hive-utils` | `hive-utils.map-vote` | bool | `false` | Vote for preferred maps |
| `hive-utils` | `hive-utils.maps` | text | empty | Preferred maps, comma-separated |
| `hive-utils` | `hive-utils.avoid` | text | empty | Maps to avoid, comma-separated |
| `hive-utils` | `hive-utils.copy-join-code` | bool | `false` | Copy custom-server join codes |
| `skin-grabber` | `skin-grabber.key` | key: VK code 0-254, 0 = unbound | `0 (unbound)` | Grab key |
| `skin-grabber` | `skin-grabber.on-key` | choice: save / wear / both | `save` | Grab key action |
| `skin-grabber` | `skin-grabber.player` | text | empty | Named action target |
| `skin-grabber` | `skin-grabber.cape` | bool | `true` | Also save capes |
