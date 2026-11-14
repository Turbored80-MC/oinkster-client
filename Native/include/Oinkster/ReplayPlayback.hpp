#pragma once

#include "Oinkster/ReplayBlockPack.hpp"
#include "Oinkster/ReplayFile.hpp"
#include "Oinkster/ReplayTimeline.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

// Turns a recording into the packet stream a client expects: Bedrock's batch
// framing, the join handshake, and pacing the recorded server packets (with
// pause and speed). Pure logic; ReplayInjector hands the packets to the game.
namespace Oinkster::ReplayPlayback {

using Clock = std::chrono::steady_clock;

// A game-packet batch: 0xFE, then (once compression is negotiated) a
// compression byte (0xFF = none), then varuint32-length-prefixed packets.
[[nodiscard]] std::vector<std::uint8_t> EncodeBatch(std::span<const std::vector<std::uint8_t>> packets, bool compressionHeader);
// nullopt when malformed or compressed with an algorithm we do not decode.
[[nodiscard]] std::optional<std::vector<std::vector<std::uint8_t>>> DecodeBatch(std::span<const std::uint8_t> message, bool compressionHeader);

// The NetworkSettings reply: zlib with the largest threshold, so the client
// never compresses (it marks every batch 0xFF) and neither do we.
[[nodiscard]] std::vector<std::uint8_t> NetworkSettingsPacket();

// Rewrites StartGame's player game mode (third field) to spectator so the
// viewer can fly through the replayed world. nullopt if it does not parse.
struct StartGameIds {
    std::int64_t uniqueId{};
    std::uint64_t runtimeId{};
};
[[nodiscard]] std::optional<std::vector<std::uint8_t>> PatchStartGameToSpectator(std::span<const std::uint8_t> packet,
    StartGameIds& ids);

// The runtime id the recording player (the "ghost", see ReplayPlayback.cpp)
// is re-created with.
inline constexpr std::uint64_t RecorderRuntimeId = 0x7FFF0001;

// minecraft:air as a hashed block network id (FNV-1a of its block-state NBT,
// which is how the game numbers blocks when block ids are hashes).
inline constexpr std::uint32_t AirBlockHash = 0xDBF44120U;

enum class Stage : std::uint8_t {
    WaitingForSettings,
    WaitingForLogin,
    WaitingForPacks,
    WaitingForPacksCompleted,
    CatchingUp,
    Playing,
    Finished,
};

struct ClientPacketResult {
    bool accepted{true};
    std::string error;
};

class Session {
public:
    // Builds the playable stream from a recording; fails if it has no world.
    [[nodiscard]] static std::optional<Session> Load(const Replay::ReadResult& replay, std::string& error);

    // A packet the client sent (header varint + body). Replies go to `out`.
    // A rejected result is fatal after any reply has been flushed.
    [[nodiscard]] ClientPacketResult OnClientPacket(std::span<const std::uint8_t> packet, Clock::time_point now,
        std::vector<std::vector<std::uint8_t>>& out);
    // Recorded packets due now, up to about budgetBytes.
    void Pump(Clock::time_point now, std::size_t budgetBytes, std::vector<std::vector<std::uint8_t>>& out);

    // In-game playback: the game's own local world has already completed its
    // handshake, so skip straight to rebuilding the recorded world.
    void BeginInjected(Clock::time_point now);
    // Moves playback to positionMicros on the clip timeline. Forward, the
    // packets in between are sent at once. Backward, the entities the replay
    // spawned are removed and the world is replayed from its start up to the
    // target (one-time join packets are not sent twice). Returns true for a
    // rewind, after which already-pumped packets must be discarded.
    bool SeekTo(std::uint64_t positionMicros, Clock::time_point now);
    void SetPaused(bool paused, Clock::time_point now);
    void SetSpeed(float speed, Clock::time_point now);

    [[nodiscard]] Stage CurrentStage() const noexcept { return stage_; }
    [[nodiscard]] bool CompressionEnabled() const noexcept { return compression_; }
    [[nodiscard]] bool Paused() const noexcept { return paused_; }
    [[nodiscard]] float Speed() const noexcept { return speed_; }
    // Playback position and length, relative to the clip start.
    [[nodiscard]] std::uint64_t PositionMicros(Clock::time_point now) const noexcept;
    [[nodiscard]] std::uint64_t LengthMicros() const noexcept { return endMicros_ > clipStart_ ? endMicros_ - clipStart_ : 0; }
    [[nodiscard]] std::size_t PacketsSent() const noexcept { return next_; }
    [[nodiscard]] std::size_t PacketCount() const noexcept { return stream_.size(); }
    // Whether the recording player was re-created, and with whose list entry.
    [[nodiscard]] bool HasGhost() const noexcept { return ghost_; }
    [[nodiscard]] bool GhostHasSkin() const noexcept { return ghostSkin_; }
    // Old-height (0-255) server chunks rewritten for the modern replay world.
    [[nodiscard]] std::size_t ModernizedChunks() const noexcept { return modernizedChunks_; }
    // The blocks the recorded server defined in StartGame (see ReplayBlockPack).
    [[nodiscard]] const std::vector<ReplayBlockPack::CustomBlock>& CustomBlocks() const noexcept { return customBlocks_; }
    // In-game playback joins through the game's own local world, which has
    // no packs. Its ResourcePacksInfo and ResourcePackStack are swapped for
    // the recorded server's, so the server's packs (cached, or downloaded
    // from the recording) are applied.
    [[nodiscard]] const std::vector<std::uint8_t>& PacksInfoPacket() const noexcept { return packsInfo_; }
    [[nodiscard]] const std::vector<std::uint8_t>& PackStackPacket() const noexcept { return packStack_; }
    // A pack download the client asks the local world for (a "send packs"
    // response, or a chunk request), answered from the recording instead.
    // True when handled: the request must then not reach the local world.
    [[nodiscard]] bool AnswerPackDownload(std::span<const std::uint8_t> packet, std::vector<std::vector<std::uint8_t>>& out) const;

    // The viewer's own runtime id: the recorded StartGame's.
    [[nodiscard]] std::uint64_t ViewerRuntimeId() const noexcept { return viewerRuntimeId_; }
    // The recording player, despawned and spawned again on demand (while
    // the viewer looks through their eyes their own head would block it).
    // eyeY is at eye height; empty when the recording has no ghost.
    [[nodiscard]] std::vector<std::uint8_t> GhostRemovePacket() const;
    [[nodiscard]] std::vector<std::uint8_t> GhostSpawnPacket(double x, double eyeY, double z, float yaw, float pitch) const;
    // The recording player's camera at the current playback position,
    // interpolated between the recorded samples (feet position, yaw, pitch).
    // Following it this way respects pause, speed and seeking exactly.
    [[nodiscard]] std::optional<Replay::CameraSample> RecorderCameraAt(Clock::time_point now) const;

    struct Mark {
        std::uint64_t positionMicros{};  // relative to the clip start
        std::string label;
    };
    [[nodiscard]] const std::vector<Mark>& Marks() const noexcept { return marks_; }

private:
    struct Packet {
        std::uint64_t t{};
        std::uint32_t id{};
        std::vector<std::uint8_t> bytes;
        // The ghost's own AddPlayer: never sent twice.
        bool ghostSpawn{};
    };
    struct PackChunk {
        std::string uuid;
        std::uint32_t index{};
        std::vector<std::uint8_t> bytes;
    };
    [[nodiscard]] double AbsoluteMicros(Clock::time_point now) const noexcept;
    // Re-creates the recording player (see ReplayPlayback.cpp, Ghost).
    void AddGhost(const Replay::ReadResult& replay, const std::array<std::uint8_t, 16>& recorderUuid,
        const std::string& name, std::uint64_t startGameMicros);
    // The ghost's held item and armor, rebuilt from the recorded inventory.
    void AddGhostEquipment(const Replay::ReadResult& replay, const Replay::ReplayTimeline& timeline, std::uint64_t spawnAt);
    void IndexForSeeking(const Replay::ReadResult& replay);
    // Whether a packet goes out on this pass (see SeekTo), tracking spawns.
    [[nodiscard]] bool Admit(std::size_t index, const Packet& packet);

    // A block the recording changed, and when it first did. A rewind does
    // not reset blocks the client already has (re-sent chunks do not
    // overwrite them), so blocks placed later would stay: those are put back
    // to air first (see SeekTo).
    struct BlockTouch {
        // Position as encoded: zigzag x, unsigned y, zigzag z.
        std::uint32_t x{};
        std::uint32_t y{};
        std::uint32_t z{};
        std::uint32_t layer{};
        std::uint32_t firstRuntimeId{};
        std::uint64_t firstT{};
    };
    std::vector<BlockTouch> blockTouches_;
    // Block ids are hashes (so AirBlockHash is air) in this recording.
    bool blockIdsHashed_{};
    std::uint64_t viewerRuntimeId_{};
    std::array<std::uint8_t, 16> ghostUuid_{};
    std::string ghostName_;
    // The recorded camera path the ghost walks (samples only).
    Replay::ReplayTimeline camera_;

    std::vector<Packet> stream_;
    std::vector<std::uint8_t> packsInfo_;
    std::vector<std::uint8_t> packStack_;
    std::vector<std::vector<std::uint8_t>> packDataInfo_;
    std::vector<PackChunk> packChunks_;
    std::uint64_t clipStart_{};
    std::uint64_t endMicros_{};
    std::size_t next_{};
    Stage stage_{Stage::WaitingForSettings};
    bool compression_{};
    bool stackSent_{};
    bool paused_{};
    bool ghost_{};
    bool ghostSkin_{};
    std::size_t modernizedChunks_{};
    std::vector<ReplayBlockPack::CustomBlock> customBlocks_;
    // Seeking: where a rewind restarts (just after StartGame), where the
    // player spawned (before it only terrain and entities are re-sent), the
    // entities spawned so far (removed on rewind), and RemoveActor packets
    // waiting to go out first.
    std::size_t afterStartGame_{};
    std::size_t spawnIndex_{};
    bool rebuilding_{};
    bool ghostSpawned_{};
    std::vector<std::int64_t> spawned_;
    std::vector<std::vector<std::uint8_t>> prefix_;
    std::vector<Mark> marks_;
    float speed_{1.0F};
    double anchorMicros_{};
    Clock::time_point anchorAt_{};
};

}
