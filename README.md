# Oinkster Client

A free, open-source client for Minecraft Bedrock on Windows. Press **K** in-game to open the menu and turn on the mods you want: FPS and CPS counters, keystrokes, zoom, freelook, a custom crosshair, armor and potion HUDs, and around fifty more.

**Current version:** 1.0.3 Beta, for Minecraft Bedrock 1.26.5203 (Windows, x64).

Website: https://oinksterclient.vercel.app
Found a bug? [Report it on the website](https://oinksterclient.vercel.app/bugs) or [open an issue](https://github.com/Turbored80-MC/oinkster-client/issues). Got an idea? [Suggest it](https://oinksterclient.vercel.app/suggestions).

## How it works

Oinkster has two parts:

- **The launcher** (repo root, C# / WinUI 3). It finds your Minecraft install, downloads the client, checks it, and loads it into the game.
- **The client** (`Native/`, C++). A DLL that runs inside Minecraft and draws its menu and HUD straight into the game's DirectX 12 frame with Dear ImGui.

Before loading anything, the launcher checks that:

1. the release metadata is signed with the Oinkster release key (`Assets/OinksterReleasePublicKey.pem`);
2. the downloaded DLL matches the signed size and SHA-256 hash;
3. Minecraft is the 64-bit Windows build the DLL was made for.

If any check fails, nothing is loaded. After a Minecraft update, the launcher waits for a matching Oinkster release.

The launcher checks for updates when it opens, every 30 minutes while it stays open, and before launching if the last check is old. A new client DLL downloads by itself (only the DLL, a few MB). A new launcher comes as a signed installer, checked the same way, which installs quietly and reopens the launcher. Releases that only change the client never make you download the launcher again.

The client never installs global keyboard or mouse hooks, and it doesn't collect anything about you. The only network request the launcher makes is to GitHub, to check for updates.

## Building

You need:

- Windows 10 or 11 (x64)
- Visual Studio 2022 with **Desktop development with C++** and **WinUI application development**
- The .NET 10 SDK
- CMake 3.24 or newer

Client DLL and its tests:

```powershell
cd Native
cmake --preset windows-x64
cmake --build --preset debug
ctest --preset debug
```

Launcher and its tests:

```powershell
dotnet build OinksterClient.csproj -p:Platform=x64
dotnet test OinksterClient.Tests/OinksterClient.Tests.csproj
dotnet run --project OinksterClient.csproj -p:Platform=x64
```

A Debug launcher build picks up the Debug DLL from `Native/out/build/x64/Debug` automatically.

While you test, the client writes a log to `%LOCALAPPDATA%\Oinkster\Client\oinkster-native.log`. It's the first place to look when something goes wrong, and it's useful to attach to bug reports.

More detail on the client's modules, settings and hooks is in [`Native/README.md`](Native/README.md).

## Credits

Several parts of the client are based on [Flarial](https://github.com/flarialmc/dll-oss), an open-source Bedrock client licensed under the AGPL-3.0: the hook registry layout, cursor handling, game-window lookup, and some game struct layouts and signatures. The source comments say where. Thank you to the Flarial team for publishing their work.

The client also uses [MinHook](https://github.com/TsudaKageyu/minhook), [Dear ImGui](https://github.com/ocornut/imgui) and [libhat](https://github.com/BasedInc/libhat).

## License

Oinkster is licensed under the [GNU Affero General Public License v3.0](LICENSE), the same license as the Flarial code it builds on. You can use, change and share it, as long as you share your changes under the same license.

Oinkster isn't an official Minecraft product, and isn't approved by or associated with Mojang Studios or Microsoft.
