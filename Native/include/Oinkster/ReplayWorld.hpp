#pragma once

#include "Oinkster/ReplayBlockPack.hpp"

#include <filesystem>
#include <span>
#include <string>
#include <vector>

// Starts a private, temporary local world for replay playback, the way
// Playback (wo55555/Playback) does: by asking the game itself to start a
// local server, through IGameServerStartup::startLocalServerAsync on the
// game's own MinecraftGame, with LevelSettings built by the game's own
// LevelSettings constructor. The world is void, creative, peaceful and not
// broadcast; ReplayInjector then replaces what it shows with the recording.
//
// Everything is resolved by unique .text signatures and checked at runtime
// (the MinecraftGame subobject's vtable must point at the matched function)
// before anything is called; the call itself runs on the game's main thread
// from a MinecraftGame::_update hook, after the original update.
namespace Oinkster::ReplayWorld {

[[nodiscard]] bool Install(std::wstring& detail) noexcept;
void Remove() noexcept;

// Whether every piece was resolved, so a temporary world can be started.
[[nodiscard]] bool Available() noexcept;
// Asks the main thread to start the replay world on its next update. Only
// acts on Minecraft's title screen; one request at a time.
void RequestStart() noexcept;
// Pending, or the last attempt's outcome (empty while none failed).
[[nodiscard]] bool Pending() noexcept;
[[nodiscard]] std::string LastError();

// The temporary world's folder name and display name (levelname.txt).
inline constexpr char WorldFolderName[] = "OinksterReplay";
inline constexpr char WorldDisplayName[] = "Oinkster Replay";

// Replay worlds under a Minecraft data root: <root>/Users/<user>/games/
// com.mojang/minecraftWorlds/OinksterReplay (the GDK layout) and
// <root>/games/com.mojang/minecraftWorlds/OinksterReplay (UWP LocalState).
// Only folders whose levelname.txt is exactly WorldDisplayName qualify, so
// a world the player made and happened to name the same is never touched.
[[nodiscard]] std::vector<std::filesystem::path> FindReplayWorlds(const std::filesystem::path& root);

// Replaces every replay world folder with a fresh one holding `pack` as its
// behavior pack (see ReplayBlockPack), so the world the game creates there
// loads it. With no files, old replay worlds are only removed. Returns how
// many folders were written; 0 with `error` set on failure.
std::size_t PrepareWorld(std::span<const ReplayBlockPack::PackFile> pack, std::string& error);

// Deletes every replay world on a worker thread, retrying for a while if
// the game still has files open. Call only when no replay is running and
// the game is out of any world. `abort` is checked before every attempt and
// stops the cleanup when a new replay starts. No-op while one is running.
void DeleteReplayWorldsAsync(bool (*abort)()) noexcept;

}
