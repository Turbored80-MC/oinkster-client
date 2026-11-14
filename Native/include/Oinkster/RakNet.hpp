#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

// The server half of RakNet as Minecraft Bedrock speaks it (protocol 11):
// offline connection handshake, datagrams and frames, reliable-ordered
// delivery with ACK/NAK and resend, and split (fragmented) messages both
// ways. Pure logic over byte buffers. Replay playback no longer runs a
// server (see ReplayInjector); only Reader/Writer are used outside tests.
namespace Oinkster::RakNet {

inline constexpr std::array<std::uint8_t, 16> Magic{
    0x00, 0xFF, 0xFF, 0x00, 0xFE, 0xFE, 0xFE, 0xFE, 0xFD, 0xFD, 0xFD, 0xFD, 0x12, 0x34, 0x56, 0x78};
inline constexpr std::uint8_t ProtocolVersion = 11;
// IPv4 + UDP headers, counted by RakNet when sizing the MTU.
inline constexpr std::size_t UdpOverhead = 28;

namespace MessageId {
inline constexpr std::uint8_t ConnectedPing = 0x00;
inline constexpr std::uint8_t UnconnectedPing = 0x01;
inline constexpr std::uint8_t UnconnectedPingOpen = 0x02;
inline constexpr std::uint8_t ConnectedPong = 0x03;
inline constexpr std::uint8_t OpenConnectionRequest1 = 0x05;
inline constexpr std::uint8_t OpenConnectionReply1 = 0x06;
inline constexpr std::uint8_t OpenConnectionRequest2 = 0x07;
inline constexpr std::uint8_t OpenConnectionReply2 = 0x08;
inline constexpr std::uint8_t ConnectionRequest = 0x09;
inline constexpr std::uint8_t ConnectionRequestAccepted = 0x10;
inline constexpr std::uint8_t NewIncomingConnection = 0x13;
inline constexpr std::uint8_t DisconnectNotification = 0x15;
inline constexpr std::uint8_t UnconnectedPong = 0x1C;
inline constexpr std::uint8_t GamePacket = 0xFE;
}

enum class Reliability : std::uint8_t {
    Unreliable = 0,
    UnreliableSequenced = 1,
    Reliable = 2,
    ReliableOrdered = 3,
    ReliableSequenced = 4,
    UnreliableAckReceipt = 5,
    ReliableAckReceipt = 6,
    ReliableOrderedAckReceipt = 7,
};
[[nodiscard]] bool IsReliable(Reliability value) noexcept;
[[nodiscard]] bool IsSequenced(Reliability value) noexcept;
[[nodiscard]] bool IsOrdered(Reliability value) noexcept;

struct Address {
    std::array<std::uint8_t, 4> ip{};
    std::uint16_t port{};
    friend bool operator==(const Address&, const Address&) = default;
};

class Writer {
public:
    void U8(std::uint8_t value) { bytes_.push_back(value); }
    void U16BE(std::uint16_t value);
    void U16LE(std::uint16_t value);
    void U24LE(std::uint32_t value);
    void U32BE(std::uint32_t value);
    void U32LE(std::uint32_t value);
    void U64BE(std::uint64_t value);
    void F32LE(float value);
    void VarU32(std::uint32_t value);
    void VarU64(std::uint64_t value);
    void Bytes(std::span<const std::uint8_t> value) { bytes_.insert(bytes_.end(), value.begin(), value.end()); }
    void WriteMagic() { Bytes(Magic); }
    void WriteAddress(const Address& address);
    [[nodiscard]] std::vector<std::uint8_t>& Data() noexcept { return bytes_; }
    [[nodiscard]] std::vector<std::uint8_t> Take() noexcept { return std::move(bytes_); }

private:
    std::vector<std::uint8_t> bytes_;
};

// Bounds-checked reader: any read past the end returns zero and clears ok().
class Reader {
public:
    explicit Reader(std::span<const std::uint8_t> bytes) noexcept : bytes_(bytes) {}
    std::uint8_t U8() noexcept;
    std::uint16_t U16BE() noexcept;
    std::uint16_t U16LE() noexcept;
    std::uint32_t U24LE() noexcept;
    std::uint32_t U32BE() noexcept;
    std::uint64_t U64BE() noexcept;
    std::uint32_t VarU32() noexcept;
    std::uint64_t VarU64() noexcept;
    std::span<const std::uint8_t> Bytes(std::size_t count) noexcept;
    bool ReadMagic() noexcept;
    std::optional<Address> ReadAddress() noexcept;
    void Skip(std::size_t count) noexcept { (void)Bytes(count); }
    [[nodiscard]] std::size_t Position() const noexcept { return position_; }
    [[nodiscard]] std::size_t Remaining() const noexcept { return bytes_.size() - position_; }
    [[nodiscard]] std::span<const std::uint8_t> Rest() const noexcept { return bytes_.subspan(position_); }
    [[nodiscard]] bool Ok() const noexcept { return ok_; }

private:
    std::span<const std::uint8_t> bytes_;
    std::size_t position_{};
    bool ok_{true};
};

struct Frame {
    Reliability reliability{Reliability::ReliableOrdered};
    std::uint32_t reliableIndex{};
    std::uint32_t sequencedIndex{};
    std::uint32_t orderIndex{};
    std::uint8_t orderChannel{};
    bool split{};
    std::uint32_t splitCount{};
    std::uint16_t splitId{};
    std::uint32_t splitIndex{};
    std::vector<std::uint8_t> body;
};

struct Datagram {
    std::uint32_t sequence{};
    std::vector<Frame> frames;
};

[[nodiscard]] std::size_t FrameHeaderSize(const Frame& frame) noexcept;
void WriteFrame(Writer& writer, const Frame& frame);
[[nodiscard]] std::optional<Datagram> ParseDatagram(std::span<const std::uint8_t> bytes);
[[nodiscard]] bool IsAckOrNak(std::span<const std::uint8_t> bytes) noexcept;
// ACK (0xC0) or NAK (0xA0) for a set of datagram sequence numbers.
[[nodiscard]] std::vector<std::uint8_t> BuildAck(bool nak, std::vector<std::uint32_t> sequences);
// Expands an ACK/NAK; ranges are bounded so a hostile packet cannot explode.
[[nodiscard]] std::vector<std::uint32_t> ParseAck(std::span<const std::uint8_t> bytes);

// Offline handshake replies.
[[nodiscard]] std::vector<std::uint8_t> BuildUnconnectedPong(std::uint64_t pingTime, std::uint64_t serverGuid, const std::string& motd);
[[nodiscard]] std::vector<std::uint8_t> BuildOpenConnectionReply1(std::uint64_t serverGuid, std::uint16_t mtu);
[[nodiscard]] std::vector<std::uint8_t> BuildOpenConnectionReply2(std::uint64_t serverGuid, const Address& client, std::uint16_t mtu);

// Reassembles split messages; bounded in count and size.
class SplitAssembler {
public:
    [[nodiscard]] std::optional<std::vector<std::uint8_t>> Add(Frame frame);

private:
    struct Pending {
        std::uint32_t count{};
        std::size_t bytes{};
        std::map<std::uint32_t, std::vector<std::uint8_t>> parts;
    };
    std::map<std::uint16_t, Pending> pending_;
};

using Clock = std::chrono::steady_clock;

// One connected client, server side. Receive() takes the client's datagrams
// and returns the application messages they complete (game packets and
// connection control are both returned; the caller answers control).
// Send() queues a reliable-ordered message; Flush() returns the datagrams to
// put on the wire now: ACKs, new data within the in-flight window, and
// resends of anything NAKed or unacknowledged for too long.
class Connection {
public:
    Connection(std::uint16_t mtu, std::size_t windowBytes = 1U << 20U) noexcept;

    [[nodiscard]] std::vector<std::vector<std::uint8_t>> Receive(std::span<const std::uint8_t> datagram, Clock::time_point now);
    void Send(std::span<const std::uint8_t> message);
    [[nodiscard]] std::vector<std::vector<std::uint8_t>> Flush(Clock::time_point now);

    [[nodiscard]] std::size_t BytesInFlight() const noexcept { return bytesInFlight_; }
    [[nodiscard]] std::size_t QueuedFrames() const noexcept { return queue_.size(); }
    [[nodiscard]] bool HasCapacity() const noexcept { return bytesInFlight_ + queuedBytes_ < windowBytes_; }
    [[nodiscard]] Clock::time_point LastReceived() const noexcept { return lastReceived_; }

private:
    // Frames of an unacknowledged datagram, rewrapped under a new sequence
    // number when resent.
    struct InFlight {
        std::vector<std::uint8_t> datagram;
        Clock::time_point sentAt;
    };
    std::size_t mtu_;
    std::size_t windowBytes_;
    std::uint32_t nextSequence_{};
    std::uint32_t nextReliableIndex_{};
    std::uint32_t nextOrderIndex_{};
    std::uint16_t nextSplitId_{};
    std::deque<Frame> queue_;
    std::size_t queuedBytes_{};
    std::map<std::uint32_t, InFlight> inFlight_;
    std::size_t bytesInFlight_{};
    std::vector<std::uint32_t> pendingAcks_;
    std::vector<std::uint32_t> resendRequested_;
    SplitAssembler assembler_;
    std::set<std::uint32_t> receivedReliable_;
    Clock::time_point lastReceived_{};
};

}
