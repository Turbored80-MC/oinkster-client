#include "Oinkster/RakNet.hpp"

#include <algorithm>
#include <bit>
#include <cstring>

namespace Oinkster::RakNet {
namespace {
constexpr std::size_t DatagramHeaderBytes = 4;
// Worst-case frame header: flags, length, reliable, ordered, split fields.
constexpr std::size_t MaxFrameHeaderBytes = 1 + 2 + 3 + 3 + 4 + 10;
constexpr std::uint32_t MaxSplitCount = 4096;
constexpr std::size_t MaxSplitBytes = 32U * 1024U * 1024U;
constexpr std::size_t MaxAckEntries = 8192;
constexpr auto ResendAfter = std::chrono::milliseconds(750);
}

bool IsReliable(Reliability value) noexcept {
    return value == Reliability::Reliable || value == Reliability::ReliableOrdered || value == Reliability::ReliableSequenced
        || value == Reliability::ReliableAckReceipt || value == Reliability::ReliableOrderedAckReceipt;
}
bool IsSequenced(Reliability value) noexcept {
    return value == Reliability::UnreliableSequenced || value == Reliability::ReliableSequenced;
}
bool IsOrdered(Reliability value) noexcept {
    return value == Reliability::UnreliableSequenced || value == Reliability::ReliableOrdered
        || value == Reliability::ReliableSequenced || value == Reliability::ReliableOrderedAckReceipt;
}

void Writer::U16BE(std::uint16_t value) { U8(static_cast<std::uint8_t>(value >> 8U)); U8(static_cast<std::uint8_t>(value)); }
void Writer::U16LE(std::uint16_t value) { U8(static_cast<std::uint8_t>(value)); U8(static_cast<std::uint8_t>(value >> 8U)); }
void Writer::U24LE(std::uint32_t value) {
    U8(static_cast<std::uint8_t>(value));
    U8(static_cast<std::uint8_t>(value >> 8U));
    U8(static_cast<std::uint8_t>(value >> 16U));
}
void Writer::U32BE(std::uint32_t value) { for (int shift = 24; shift >= 0; shift -= 8) U8(static_cast<std::uint8_t>(value >> shift)); }
void Writer::U32LE(std::uint32_t value) { for (int shift = 0; shift < 32; shift += 8) U8(static_cast<std::uint8_t>(value >> shift)); }
void Writer::U64BE(std::uint64_t value) { for (int shift = 56; shift >= 0; shift -= 8) U8(static_cast<std::uint8_t>(value >> shift)); }
void Writer::F32LE(float value) { U32LE(std::bit_cast<std::uint32_t>(value)); }
void Writer::VarU32(std::uint32_t value) {
    while (value >= 0x80U) {
        U8(static_cast<std::uint8_t>(value | 0x80U));
        value >>= 7U;
    }
    U8(static_cast<std::uint8_t>(value));
}
void Writer::VarU64(std::uint64_t value) {
    while (value >= 0x80U) {
        U8(static_cast<std::uint8_t>(value | 0x80U));
        value >>= 7U;
    }
    U8(static_cast<std::uint8_t>(value));
}
void Writer::WriteAddress(const Address& address) {
    U8(4);
    for (const auto byte : address.ip) U8(static_cast<std::uint8_t>(~byte));
    U16BE(address.port);
}

std::span<const std::uint8_t> Reader::Bytes(std::size_t count) noexcept {
    if (!ok_ || count > Remaining()) {
        ok_ = false;
        position_ = bytes_.size();
        return {};
    }
    const auto result = bytes_.subspan(position_, count);
    position_ += count;
    return result;
}
std::uint8_t Reader::U8() noexcept {
    const auto bytes = Bytes(1);
    return bytes.empty() ? 0 : bytes[0];
}
std::uint16_t Reader::U16BE() noexcept {
    const auto bytes = Bytes(2);
    return bytes.empty() ? 0 : static_cast<std::uint16_t>((bytes[0] << 8U) | bytes[1]);
}
std::uint16_t Reader::U16LE() noexcept {
    const auto bytes = Bytes(2);
    return bytes.empty() ? 0 : static_cast<std::uint16_t>((bytes[1] << 8U) | bytes[0]);
}
std::uint32_t Reader::U24LE() noexcept {
    const auto bytes = Bytes(3);
    return bytes.empty() ? 0 : static_cast<std::uint32_t>(bytes[0] | (bytes[1] << 8U) | (bytes[2] << 16U));
}
std::uint32_t Reader::U32BE() noexcept {
    const auto bytes = Bytes(4);
    if (bytes.empty()) return 0;
    return (static_cast<std::uint32_t>(bytes[0]) << 24U) | (static_cast<std::uint32_t>(bytes[1]) << 16U)
        | (static_cast<std::uint32_t>(bytes[2]) << 8U) | bytes[3];
}
std::uint64_t Reader::U64BE() noexcept {
    const auto bytes = Bytes(8);
    std::uint64_t value{};
    for (const auto byte : bytes) value = (value << 8U) | byte;
    return value;
}
std::uint32_t Reader::VarU32() noexcept {
    std::uint32_t value{};
    for (unsigned index = 0; index < 5; ++index) {
        const auto byte = U8();
        if (!ok_) return 0;
        value |= static_cast<std::uint32_t>(byte & 0x7FU) << (7U * index);
        if ((byte & 0x80U) == 0) return value;
    }
    ok_ = false;
    return 0;
}
std::uint64_t Reader::VarU64() noexcept {
    std::uint64_t value{};
    for (unsigned index = 0; index < 10; ++index) {
        const auto byte = U8();
        if (!ok_) return 0;
        value |= static_cast<std::uint64_t>(byte & 0x7FU) << (7U * index);
        if ((byte & 0x80U) == 0) return value;
    }
    ok_ = false;
    return 0;
}
bool Reader::ReadMagic() noexcept {
    const auto bytes = Bytes(Magic.size());
    return !bytes.empty() && std::equal(bytes.begin(), bytes.end(), Magic.begin());
}
std::optional<Address> Reader::ReadAddress() noexcept {
    const auto version = U8();
    Address address;
    if (version == 4) {
        const auto ip = Bytes(4);
        if (ip.empty()) return std::nullopt;
        for (std::size_t index = 0; index < 4; ++index) address.ip[index] = static_cast<std::uint8_t>(~ip[index]);
        address.port = U16BE();
    } else if (version == 6) {
        (void)U16LE();
        address.port = U16BE();
        Skip(4 + 16 + 4);
    } else {
        ok_ = false;
    }
    if (!ok_) return std::nullopt;
    return address;
}

std::size_t FrameHeaderSize(const Frame& frame) noexcept {
    std::size_t size = 1 + 2;
    if (IsReliable(frame.reliability)) size += 3;
    if (IsSequenced(frame.reliability)) size += 3;
    if (IsOrdered(frame.reliability)) size += 4;
    if (frame.split) size += 10;
    return size;
}

void WriteFrame(Writer& writer, const Frame& frame) {
    writer.U8(static_cast<std::uint8_t>((static_cast<std::uint8_t>(frame.reliability) << 5U) | (frame.split ? 0x10U : 0U)));
    writer.U16BE(static_cast<std::uint16_t>(frame.body.size() * 8U));
    if (IsReliable(frame.reliability)) writer.U24LE(frame.reliableIndex);
    if (IsSequenced(frame.reliability)) writer.U24LE(frame.sequencedIndex);
    if (IsOrdered(frame.reliability)) {
        writer.U24LE(frame.orderIndex);
        writer.U8(frame.orderChannel);
    }
    if (frame.split) {
        writer.U32BE(frame.splitCount);
        writer.U16BE(frame.splitId);
        writer.U32BE(frame.splitIndex);
    }
    writer.Bytes(frame.body);
}

bool IsAckOrNak(std::span<const std::uint8_t> bytes) noexcept {
    return !bytes.empty() && (bytes[0] & 0x80U) != 0 && (bytes[0] & 0x60U) != 0;
}

std::optional<Datagram> ParseDatagram(std::span<const std::uint8_t> bytes) {
    if (bytes.empty() || (bytes[0] & 0x80U) == 0 || IsAckOrNak(bytes)) return std::nullopt;
    Reader reader(bytes);
    reader.Skip(1);
    Datagram datagram;
    datagram.sequence = reader.U24LE();
    while (reader.Ok() && reader.Remaining() > 0) {
        Frame frame;
        const auto flags = reader.U8();
        frame.reliability = static_cast<Reliability>(flags >> 5U);
        frame.split = (flags & 0x10U) != 0;
        const auto length = (static_cast<std::size_t>(reader.U16BE()) + 7U) / 8U;
        if (IsReliable(frame.reliability)) frame.reliableIndex = reader.U24LE();
        if (IsSequenced(frame.reliability)) frame.sequencedIndex = reader.U24LE();
        if (IsOrdered(frame.reliability)) {
            frame.orderIndex = reader.U24LE();
            frame.orderChannel = reader.U8();
        }
        if (frame.split) {
            frame.splitCount = reader.U32BE();
            frame.splitId = reader.U16BE();
            frame.splitIndex = reader.U32BE();
        }
        const auto body = reader.Bytes(length);
        if (!reader.Ok()) return std::nullopt;
        frame.body.assign(body.begin(), body.end());
        datagram.frames.push_back(std::move(frame));
    }
    if (!reader.Ok()) return std::nullopt;
    return datagram;
}

std::vector<std::uint8_t> BuildAck(bool nak, std::vector<std::uint32_t> sequences) {
    std::ranges::sort(sequences);
    const auto duplicates = std::ranges::unique(sequences);
    sequences.erase(duplicates.begin(), duplicates.end());
    Writer records;
    std::uint16_t count{};
    for (std::size_t index = 0; index < sequences.size();) {
        auto end = index;
        while (end + 1 < sequences.size() && sequences[end + 1] == sequences[end] + 1) ++end;
        if (end == index) {
            records.U8(1);
            records.U24LE(sequences[index]);
        } else {
            records.U8(0);
            records.U24LE(sequences[index]);
            records.U24LE(sequences[end]);
        }
        ++count;
        index = end + 1;
    }
    Writer writer;
    writer.U8(nak ? 0xA0 : 0xC0);
    writer.U16BE(count);
    writer.Bytes(records.Data());
    return writer.Take();
}

std::vector<std::uint32_t> ParseAck(std::span<const std::uint8_t> bytes) {
    std::vector<std::uint32_t> result;
    Reader reader(bytes);
    reader.Skip(1);
    const auto count = reader.U16BE();
    for (std::uint16_t record = 0; record < count && reader.Ok(); ++record) {
        const bool single = reader.U8() != 0;
        const auto start = reader.U24LE();
        const auto end = single ? start : reader.U24LE();
        if (!reader.Ok() || end < start) break;
        for (auto sequence = start; sequence <= end && result.size() < MaxAckEntries; ++sequence) result.push_back(sequence);
    }
    return result;
}

std::vector<std::uint8_t> BuildUnconnectedPong(std::uint64_t pingTime, std::uint64_t serverGuid, const std::string& motd) {
    Writer writer;
    writer.U8(MessageId::UnconnectedPong);
    writer.U64BE(pingTime);
    writer.U64BE(serverGuid);
    writer.WriteMagic();
    writer.U16BE(static_cast<std::uint16_t>(motd.size()));
    writer.Bytes(std::span(reinterpret_cast<const std::uint8_t*>(motd.data()), motd.size()));
    return writer.Take();
}

std::vector<std::uint8_t> BuildOpenConnectionReply1(std::uint64_t serverGuid, std::uint16_t mtu) {
    Writer writer;
    writer.U8(MessageId::OpenConnectionReply1);
    writer.WriteMagic();
    writer.U64BE(serverGuid);
    writer.U8(0);
    writer.U16BE(mtu);
    return writer.Take();
}

std::vector<std::uint8_t> BuildOpenConnectionReply2(std::uint64_t serverGuid, const Address& client, std::uint16_t mtu) {
    Writer writer;
    writer.U8(MessageId::OpenConnectionReply2);
    writer.WriteMagic();
    writer.U64BE(serverGuid);
    writer.WriteAddress(client);
    writer.U16BE(mtu);
    writer.U8(0);
    return writer.Take();
}

std::optional<std::vector<std::uint8_t>> SplitAssembler::Add(Frame frame) {
    if (frame.splitCount == 0 || frame.splitCount > MaxSplitCount || frame.splitIndex >= frame.splitCount) return std::nullopt;
    if (!pending_.contains(frame.splitId) && pending_.size() >= 16) pending_.erase(pending_.begin());
    auto& pending = pending_[frame.splitId];
    if (pending.count == 0) pending.count = frame.splitCount;
    if (pending.count != frame.splitCount) {
        pending_.erase(frame.splitId);
        return std::nullopt;
    }
    if (pending.parts.contains(frame.splitIndex)) return std::nullopt;
    pending.bytes += frame.body.size();
    if (pending.bytes > MaxSplitBytes) {
        pending_.erase(frame.splitId);
        return std::nullopt;
    }
    pending.parts.emplace(frame.splitIndex, std::move(frame.body));
    if (pending.parts.size() < pending.count) return std::nullopt;
    std::vector<std::uint8_t> message;
    message.reserve(pending.bytes);
    for (auto& [index, part] : pending.parts) message.insert(message.end(), part.begin(), part.end());
    pending_.erase(frame.splitId);
    return message;
}

Connection::Connection(std::uint16_t mtu, std::size_t windowBytes) noexcept
    : mtu_(std::clamp<std::size_t>(mtu, 400, 1500)), windowBytes_(windowBytes) {}

std::vector<std::vector<std::uint8_t>> Connection::Receive(std::span<const std::uint8_t> bytes, Clock::time_point now) {
    std::vector<std::vector<std::uint8_t>> messages;
    lastReceived_ = now;
    if (IsAckOrNak(bytes)) {
        const bool nak = (bytes[0] & 0x20U) != 0 && (bytes[0] & 0x40U) == 0;
        for (const auto sequence : ParseAck(bytes)) {
            if (nak) {
                resendRequested_.push_back(sequence);
                continue;
            }
            if (const auto found = inFlight_.find(sequence); found != inFlight_.end()) {
                bytesInFlight_ -= found->second.datagram.size();
                inFlight_.erase(found);
            }
        }
        return messages;
    }
    auto datagram = ParseDatagram(bytes);
    if (!datagram) return messages;
    pendingAcks_.push_back(datagram->sequence);
    for (auto& frame : datagram->frames) {
        if (IsReliable(frame.reliability)) {
            // Drop duplicates the client resent because our ACK was late.
            if (receivedReliable_.contains(frame.reliableIndex)) continue;
            receivedReliable_.insert(frame.reliableIndex);
            while (receivedReliable_.size() > 4096) receivedReliable_.erase(receivedReliable_.begin());
        }
        if (frame.split) {
            if (auto message = assembler_.Add(std::move(frame))) messages.push_back(std::move(*message));
        } else if (!frame.body.empty()) {
            messages.push_back(std::move(frame.body));
        }
    }
    return messages;
}

void Connection::Send(std::span<const std::uint8_t> message) {
    const auto maxBody = mtu_ - UdpOverhead - DatagramHeaderBytes - MaxFrameHeaderBytes;
    const auto orderIndex = nextOrderIndex_++;
    if (message.size() <= maxBody) {
        Frame frame;
        frame.reliableIndex = nextReliableIndex_++;
        frame.orderIndex = orderIndex;
        frame.body.assign(message.begin(), message.end());
        queuedBytes_ += frame.body.size() + FrameHeaderSize(frame);
        queue_.push_back(std::move(frame));
        return;
    }
    const auto count = static_cast<std::uint32_t>((message.size() + maxBody - 1) / maxBody);
    const auto splitId = nextSplitId_++;
    for (std::uint32_t index = 0; index < count; ++index) {
        Frame frame;
        frame.reliableIndex = nextReliableIndex_++;
        frame.orderIndex = orderIndex;
        frame.split = true;
        frame.splitCount = count;
        frame.splitId = splitId;
        frame.splitIndex = index;
        const auto offset = static_cast<std::size_t>(index) * maxBody;
        const auto length = std::min(maxBody, message.size() - offset);
        frame.body.assign(message.begin() + static_cast<std::ptrdiff_t>(offset),
            message.begin() + static_cast<std::ptrdiff_t>(offset + length));
        queuedBytes_ += frame.body.size() + FrameHeaderSize(frame);
        queue_.push_back(std::move(frame));
    }
}

std::vector<std::vector<std::uint8_t>> Connection::Flush(Clock::time_point now) {
    std::vector<std::vector<std::uint8_t>> out;
    if (!pendingAcks_.empty()) {
        out.push_back(BuildAck(false, std::move(pendingAcks_)));
        pendingAcks_.clear();
    }
    const auto wrap = [&](std::span<const std::uint8_t> frames) {
        Writer writer;
        writer.U8(0x84);
        const auto sequence = nextSequence_++ & 0xFFFFFFU;
        writer.U24LE(sequence);
        writer.Bytes(frames);
        auto datagram = writer.Take();
        bytesInFlight_ += frames.size();
        inFlight_[sequence] = {std::vector<std::uint8_t>(frames.begin(), frames.end()), now};
        out.push_back(std::move(datagram));
    };
    // Resend what was NAKed or has gone unacknowledged, under new sequence numbers.
    std::vector<std::uint32_t> resend = std::move(resendRequested_);
    resendRequested_.clear();
    for (const auto& [sequence, flight] : inFlight_) {
        if (now - flight.sentAt >= ResendAfter) resend.push_back(sequence);
    }
    std::ranges::sort(resend);
    const auto duplicates = std::ranges::unique(resend);
    resend.erase(duplicates.begin(), duplicates.end());
    for (const auto sequence : resend) {
        const auto found = inFlight_.find(sequence);
        if (found == inFlight_.end()) continue;
        auto frames = std::move(found->second.datagram);
        bytesInFlight_ -= frames.size();
        inFlight_.erase(found);
        wrap(frames);
    }
    // New data, packed into MTU-sized datagrams while the window has room.
    const auto maxDatagram = mtu_ - UdpOverhead;
    Writer frames;
    while (!queue_.empty() && bytesInFlight_ < windowBytes_) {
        const auto& frame = queue_.front();
        const auto size = FrameHeaderSize(frame) + frame.body.size();
        if (!frames.Data().empty() && DatagramHeaderBytes + frames.Data().size() + size > maxDatagram) {
            wrap(frames.Data());
            frames.Data().clear();
            continue;
        }
        WriteFrame(frames, frame);
        queuedBytes_ -= size;
        queue_.pop_front();
    }
    if (!frames.Data().empty()) wrap(frames.Data());
    return out;
}

}
