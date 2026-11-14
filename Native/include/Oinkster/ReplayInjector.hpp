#pragma once

#include "Oinkster/Events.hpp"
#include "Oinkster/ReplayCameraPath.hpp"
#include "Oinkster/ReplayPlayback.hpp"

#include <array>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// In-game replay playback, the approach Playback (wo55555/Playback) takes:
// no network server. The viewer opens one of their own local worlds; the
// game's integrated server completes the real join handshake, and from the
// first packet after its ResourcePackStack every server->client packet on
// that connection is dropped and the recording is handed to the game in its
// place, at the NetworkConnection::receivePacket hook (ReplayPacketBridge).
// The client therefore builds its world from the recorded StartGame (patched
// to spectator), chunks and entities, exactly as it would from a server.
namespace Oinkster::ReplayInjector {

using Clock = ReplayPlayback::Clock;

enum class Phase : std::uint8_t {
    Idle,
    Loading,
    WaitingForWorld,
    JoiningWorld,
    Playing,
    Finished,
    Failed,
};

enum class Verdict : std::uint8_t { Pass, Drop };

// A player in the replayed world, as the recording has moved them so far.
struct Player {
    std::uint64_t runtimeId{};
    std::string name;
    Vec3 eye{};
    float yaw{};
    float pitch{};
    // The person who recorded (re-created as the "ghost").
    bool recorder{};
};

// Players the replay has spawned, from AddPlayer and MovePlayer as they are
// handed to the game. Pure logic; the poses are interpolated between the
// last two updates so a camera following one moves smoothly. Only real
// players count: those on the server's player list with an Xbox id (XUID).
// Servers such as The Hive also spawn NPCs (hub characters, cosmetic shop
// displays) as "players" with list entries of their own, but with XUID "0".
class PlayerTracker {
public:
    void Observe(std::span<const std::uint8_t> packet, Clock::time_point now);
    void Clear() noexcept {
        players_.clear();
        lists_.clear();
    }
    [[nodiscard]] std::vector<Player> Players() const;
    // Where `runtimeId` is at `now`, one update behind, eased between the
    // last two updates.
    [[nodiscard]] std::optional<Player> PoseAt(std::uint64_t runtimeId, Clock::time_point now) const;

private:
    struct Tracked {
        Player current;
        Player previous;
        Clock::time_point currentAt{};
        Clock::time_point previousAt{};
        std::array<std::uint8_t, 16> uuid{};
        bool listed{};
    };
    [[nodiscard]] bool Listed(const std::array<std::uint8_t, 16>& uuid) const;
    // Moves `tracked` to `player`, eased from its pose at `now`.
    void MoveTo(Tracked& tracked, Player player, Clock::time_point now);
    std::vector<Tracked> players_;
    // Every PlayerList body so far (bounded), searched for a player's UUID:
    // list entries are impractical to walk because of the skins in them.
    std::vector<std::uint8_t> lists_;
};

// A player's name for our UI: servers send name tags with colour codes
// (section sign + one character), a second line and icon-font glyphs (the
// Unicode private use area). Keeps the first line's plain text.
[[nodiscard]] std::string CleanPlayerName(std::string_view raw);

// A MovePlayer for the viewer (mode reset, so the client snaps to it).
[[nodiscard]] std::vector<std::uint8_t> ViewerMovePacket(std::uint64_t viewerRuntimeId, const Vec3& eye, float yaw, float pitch);
// Where to put the viewer to see `player`: a few blocks behind and above.
[[nodiscard]] Player BehindView(const Player& player);
// The player under the look ray from `eye`, if any is within reach.
[[nodiscard]] std::optional<Player> PlayerAlongRay(std::span<const Player> players, const Vec3& eye, float yaw, float pitch);

struct Status {
    Phase phase{Phase::Idle};
    bool running{};
    bool ready{};
    bool inWorld{};
    bool playing{};
    bool paused{};
    float speed{1.0F};
    std::uint64_t positionMicros{};
    std::uint64_t lengthMicros{};
    // Name of the player whose view is being followed, if any.
    std::optional<std::string> spectating;
    // Camera keyframes (see ReplayCameraPath) and whether the camera is
    // flying along them.
    std::size_t cameraKeyframes{};
    bool cameraPathPlaying{};
    std::string phaseText;
    std::string error;
    std::vector<ReplayPlayback::Session::Mark> marks;
};

// Pure state machine. Not thread-safe; the global functions below lock.
class Controller {
public:
    void BeginLoading();
    void Loaded(ReplayPlayback::Session session);
    void Fail(std::string error);
    void Reset();

    // One real server->client packet on `connection`. sessionStart marks the
    // first packet of a new connection; localWorld whether the world being
    // loaded is the game's own integrated server.
    [[nodiscard]] Verdict OnServerPacket(std::uintptr_t connection, std::uint32_t id, bool sessionStart,
        bool localWorld, Clock::time_point now);
    // A packet the client sent the local world (seen on the server's side
    // of the connection) while joining: pack downloads are answered from the
    // recording and dropped.
    [[nodiscard]] Verdict OnClientPacket(std::span<const std::uint8_t> packet);
    // The next recorded packet to hand the game on `connection`, if one is due.
    [[nodiscard]] std::optional<std::vector<std::uint8_t>> Next(std::uintptr_t connection, Clock::time_point now);

    void SetPaused(bool paused, Clock::time_point now);
    void SetSpeed(float speed, Clock::time_point now);
    void Seek(std::uint64_t positionMicros, Clock::time_point now);
    [[nodiscard]] Status Snapshot(Clock::time_point now) const;
    [[nodiscard]] Phase CurrentPhase() const noexcept { return phase_; }
    [[nodiscard]] std::vector<Player> Players() const { return tracker_.Players(); }
    // Moves the viewer to look at the player once.
    bool TeleportTo(std::uint64_t runtimeId, Clock::time_point now);
    // Keeps the viewer's camera at the player's eyes, facing where they face,
    // until stopped (nullopt) or the player is gone. The recording player is
    // despawned meanwhile so their head does not fill the view, and spawned
    // again (with what they hold and wear) afterwards; for anyone else the
    // camera sits just in front of their face.
    void Spectate(std::optional<std::uint64_t> runtimeId, Clock::time_point now);
    [[nodiscard]] std::optional<std::uint64_t> Spectating() const noexcept { return spectating_; }
    // The replay's camera keyframes. While the path plays, the viewer's
    // camera follows it (following a player stops it, and it stops that).
    void SetCameraPath(ReplayCameraPath::Path path);
    [[nodiscard]] const ReplayCameraPath::Path& CameraPath() const noexcept { return cameraPath_; }
    void PlayCameraPath(bool playing, Clock::time_point now);
    [[nodiscard]] bool PlayingCameraPath() const noexcept { return pathPlaying_; }
    // Ends the replay: the game is sent a Disconnect, which returns it to the
    // menu, then the controller resets.
    void Exit();

private:
    std::optional<ReplayPlayback::Session> session_;
    std::deque<std::vector<std::uint8_t>> pending_;
    std::uintptr_t connection_{};
    Phase phase_{Phase::Idle};
    bool stackSeen_{};
    bool exiting_{};
    std::string error_;
    void HideGhost();
    void ShowGhost(Clock::time_point now);

    PlayerTracker tracker_;
    std::optional<std::uint64_t> spectating_;
    Clock::time_point lastFollow_{};
    bool ghostHidden_{};
    ReplayCameraPath::Path cameraPath_;
    bool pathPlaying_{};
    // The recording player's latest held item and armor, re-sent when they
    // are spawned again.
    std::vector<std::uint8_t> ghostEquipment_;
    std::vector<std::uint8_t> ghostArmor_;
};

// Loads `replay` on a worker thread and waits for the next local world.
[[nodiscard]] bool Watch(const std::filesystem::path& replay, std::string& error);
// Where a replay's camera keyframes are saved: next to it, <name>.camera.json.
[[nodiscard]] std::filesystem::path CameraPathFile(const std::filesystem::path& replay);
void Stop() noexcept;
// Stop, then wait for a load still in progress. Teardown only: the wait can
// take as long as reading the file.
void Shutdown() noexcept;
// Loading, waiting for a world, or playing one.
[[nodiscard]] bool Running() noexcept;
[[nodiscard]] Status CurrentStatus();
void SetPaused(bool paused) noexcept;
void SetSpeed(float speed) noexcept;
// Position on the clip timeline (see Session::SeekTo).
void Seek(std::uint64_t positionMicros) noexcept;
[[nodiscard]] std::vector<Player> Players();
void TeleportTo(std::uint64_t runtimeId) noexcept;
void Spectate(std::optional<std::uint64_t> runtimeId) noexcept;
// The replay file being watched (empty when none).
[[nodiscard]] std::filesystem::path WatchedReplay();
// The watched replay's keyframes; setting them also saves them.
[[nodiscard]] ReplayCameraPath::Path CameraPath();
void SetCameraPath(ReplayCameraPath::Path path) noexcept;
void PlayCameraPath(bool playing) noexcept;
// Ends the replay and leaves its world (see Controller::Exit).
void Exit() noexcept;

// Packet-hook side (ReplayPacketBridge).
[[nodiscard]] Verdict OnServerPacket(std::uintptr_t connection, std::uint32_t id, bool sessionStart) noexcept;
[[nodiscard]] bool NextPacket(std::uintptr_t connection, std::vector<std::uint8_t>& out) noexcept;
// Client->server pack packets (ResourcePackClientResponse, chunk requests);
// only these ids are worth copying.
[[nodiscard]] bool WantsClientPacket(std::uint32_t id) noexcept;
[[nodiscard]] Verdict OnClientPacket(std::span<const std::uint8_t> packet) noexcept;
// Diagnostics: the local world's own Disconnect (kick) packet, which is
// dropped like everything else it sends once the recording plays.
void NoteDroppedDisconnect(std::span<const std::uint8_t> packet) noexcept;

}
