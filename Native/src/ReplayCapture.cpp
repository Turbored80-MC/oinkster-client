#include "Oinkster/ReplayCapture.hpp"

#include <algorithm>

namespace Oinkster::ReplayCapture {

std::optional<PacketHeader> ParseHeader(std::span<const std::uint8_t> bytes) noexcept {
    std::uint32_t value{};
    for (std::size_t index = 0; index < bytes.size() && index < 5; ++index) {
        const auto byte = bytes[index];
        if (index == 4 && (byte & 0xF0U) != 0) return std::nullopt;
        value |= static_cast<std::uint32_t>(byte & 0x7FU) << (7U * index);
        if ((byte & 0x80U) == 0) return PacketHeader{value & 0x3FFU, value, index + 1};
    }
    return std::nullopt;
}

bool IsServerToClientOnly(std::uint32_t id) noexcept {
    using namespace PacketId;
    switch (id) {
    case PlayStatus: case ServerToClientHandshake: case ResourcePacksInfo: case ResourcePackStack:
    case StartGame: case LevelChunk: case NetworkChunkPublisherUpdate: case NetworkSettings:
        return true;
    default:
        return false;
    }
}

bool IsClientToServerOnly(std::uint32_t id) noexcept {
    using namespace PacketId;
    switch (id) {
    case Login: case ClientToServerHandshake: case ResourcePackClientResponse: case RequestChunkRadius:
    case SetLocalPlayerAsInitialized: case ClientCacheStatus: case PlayerAuthInput: case RequestNetworkSettings:
        return true;
    default:
        return false;
    }
}

bool IsJoinSequence(std::uint32_t id) noexcept {
    using namespace PacketId;
    return id == NetworkSettings || id == PlayStatus || id == ServerToClientHandshake
        || id == ResourcePacksInfo || id == ResourcePackStack || id == StartGame;
}

Classification ConnectionClassifier::Observe(std::uintptr_t key, std::uint32_t packetId) noexcept {
    if (key == 0) return {};
    ++clock_;
    auto found = std::ranges::find(entries_, key, &Entry::key);
    if (found == entries_.end()) {
        found = std::ranges::min_element(entries_, {}, &Entry::lastSeen);
        *found = Entry{key, Direction::Unknown, 0, clock_};
    }
    found->lastSeen = clock_;
    if (found->direction == Direction::Unknown) {
        if (IsServerToClientOnly(packetId)) {
            found->direction = Direction::ServerToClient;
            found->session = nextSession_++;
            return {Direction::ServerToClient, true, IsJoinSequence(packetId), found->session};
        }
        if (IsClientToServerOnly(packetId)) found->direction = Direction::ClientToServer;
        return {found->direction, false, false, 0};
    }
    return {found->direction, false, false, found->session};
}

void ConnectionClassifier::Forget(std::uintptr_t key) noexcept {
    if (key == 0) return;
    if (const auto found = std::ranges::find(entries_, key, &Entry::key); found != entries_.end()) *found = {};
}

void ConnectionClassifier::Reset() noexcept {
    entries_ = {};
    clock_ = 0;
}

}
