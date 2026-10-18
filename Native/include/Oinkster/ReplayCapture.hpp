#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

// Pure logic behind ReplayPacketBridge: packet-header parsing and deciding
// which direction a game connection carries. Unit-tested; no game access.
namespace Oinkster::ReplayCapture {

// Bedrock game packet ids used by the capture and the replay player.
namespace PacketId {
inline constexpr std::uint32_t Login = 1;
inline constexpr std::uint32_t PlayStatus = 2;
inline constexpr std::uint32_t ServerToClientHandshake = 3;
inline constexpr std::uint32_t ClientToServerHandshake = 4;
inline constexpr std::uint32_t Disconnect = 5;
inline constexpr std::uint32_t ResourcePacksInfo = 6;
inline constexpr std::uint32_t ResourcePackStack = 7;
inline constexpr std::uint32_t ResourcePackClientResponse = 8;
inline constexpr std::uint32_t StartGame = 11;
inline constexpr std::uint32_t LevelChunk = 58;
inline constexpr std::uint32_t RequestChunkRadius = 69;
inline constexpr std::uint32_t SetLocalPlayerAsInitialized = 113;
inline constexpr std::uint32_t NetworkChunkPublisherUpdate = 121;
inline constexpr std::uint32_t ClientCacheStatus = 129;
inline constexpr std::uint32_t NetworkSettings = 143;
inline constexpr std::uint32_t PlayerAuthInput = 144;
inline constexpr std::uint32_t RequestNetworkSettings = 193;
}

// The game packet header is an unsigned varint: bits 0-9 are the packet id,
// bits 10-13 the sender/target sub-client ids (split-screen).
struct PacketHeader {
    std::uint32_t id{};
    std::uint32_t raw{};
    std::size_t length{};
};
[[nodiscard]] std::optional<PacketHeader> ParseHeader(std::span<const std::uint8_t> bytes) noexcept;

// Ids only a server ever sends / only a client ever sends. A connection's
// direction is decided by the first id it carries from either set; until
// then it is Unknown and nothing on it is captured.
[[nodiscard]] bool IsServerToClientOnly(std::uint32_t id) noexcept;
[[nodiscard]] bool IsClientToServerOnly(std::uint32_t id) noexcept;
// Ids that begin a join: seeing one of these first means the whole session
// is captured from its start.
[[nodiscard]] bool IsJoinSequence(std::uint32_t id) noexcept;

enum class Direction : std::uint8_t { Unknown, ServerToClient, ClientToServer };

struct Classification {
    Direction direction{Direction::Unknown};
    bool sessionStart{};
    bool joinedFromStart{};
    std::uint32_t session{};
};

// Tracks a small, bounded set of live connections by an opaque key. Keys are
// never dereferenced. The least recently seen entry is recycled when full.
class ConnectionClassifier {
public:
    [[nodiscard]] Classification Observe(std::uintptr_t key, std::uint32_t packetId) noexcept;
    // A disconnected NetworkConnection may later be reused at the same
    // address. Forget it so the next packet starts a new capture session.
    void Forget(std::uintptr_t key) noexcept;
    void Reset() noexcept;

private:
    struct Entry {
        std::uintptr_t key{};
        Direction direction{Direction::Unknown};
        std::uint32_t session{};
        std::uint64_t lastSeen{};
    };
    std::array<Entry, 8> entries_{};
    std::uint64_t clock_{};
    std::uint32_t nextSession_{1};
};

}
