#include "Oinkster/ReplayFile.hpp"

#include <Windows.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <system_error>

namespace Oinkster::Replay {
namespace {
constexpr std::array<std::uint8_t, 8> FileMagic{'O', 'I', 'N', 'K', 'R', 'P', 'L', 'Y'};
constexpr std::array<std::uint8_t, 4> RecordMagic{'R', 'C', 'H', 'K'};
constexpr std::uint32_t HeaderBytes = 32;
constexpr std::uint32_t RecordHeaderBytes = 24;

void Put16(std::span<std::uint8_t> bytes, std::size_t offset, std::uint16_t value) noexcept {
    bytes[offset] = static_cast<std::uint8_t>(value);
    bytes[offset + 1] = static_cast<std::uint8_t>(value >> 8U);
}

void Put32(std::span<std::uint8_t> bytes, std::size_t offset, std::uint32_t value) noexcept {
    for (unsigned shift = 0; shift < 32; shift += 8) bytes[offset++] = static_cast<std::uint8_t>(value >> shift);
}

void Put64(std::span<std::uint8_t> bytes, std::size_t offset, std::uint64_t value) noexcept {
    for (unsigned shift = 0; shift < 64; shift += 8) bytes[offset++] = static_cast<std::uint8_t>(value >> shift);
}

std::uint16_t Get16(std::span<const std::uint8_t> bytes, std::size_t offset) noexcept {
    return static_cast<std::uint16_t>(bytes[offset])
        | static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[offset + 1]) << 8U);
}

std::uint32_t Get32(std::span<const std::uint8_t> bytes, std::size_t offset) noexcept {
    std::uint32_t value{};
    for (unsigned shift = 0; shift < 32; shift += 8) value |= static_cast<std::uint32_t>(bytes[offset++]) << shift;
    return value;
}

std::uint64_t Get64(std::span<const std::uint8_t> bytes, std::size_t offset) noexcept {
    std::uint64_t value{};
    for (unsigned shift = 0; shift < 64; shift += 8) value |= static_cast<std::uint64_t>(bytes[offset++]) << shift;
    return value;
}

std::uint32_t Checksum(std::span<const std::uint8_t> payload) noexcept {
    std::uint32_t hash = 2166136261U;
    for (const auto byte : payload) {
        hash ^= byte;
        hash *= 16777619U;
    }
    return hash;
}

bool WriteAll(std::ofstream& stream, std::span<const std::uint8_t> bytes) {
    stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return stream.good();
}

bool WriteRecord(std::ofstream& stream, RecordKind kind, std::uint64_t timestampMicros,
    std::span<const std::uint8_t> payload) {
    std::array<std::uint8_t, RecordHeaderBytes> header{};
    std::copy(RecordMagic.begin(), RecordMagic.end(), header.begin());
    Put16(header, 4, static_cast<std::uint16_t>(kind));
    Put64(header, 8, timestampMicros);
    Put32(header, 16, static_cast<std::uint32_t>(payload.size()));
    Put32(header, 20, Checksum(payload));
    return WriteAll(stream, header) && WriteAll(stream, payload);
}

// Rename into place, falling back to copy where a packaged process may not
// rename (see Writer::Finalize).
bool Publish(const std::filesystem::path& partial, const std::filesystem::path& destination, std::string& error) {
    if (MoveFileExW(partial.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING)) return true;
    const auto moveError = GetLastError();
    if (!CopyFileW(partial.c_str(), destination.c_str(), FALSE)) {
        error = "could not publish the completed replay file (move error " + std::to_string(moveError)
            + ", copy error " + std::to_string(GetLastError()) + ")";
        return false;
    }
    (void)DeleteFileW(partial.c_str());
    return true;
}
}

Writer::~Writer() { Close(); }

bool Writer::Open(const std::filesystem::path& destination, std::uint64_t startedUnixMillis,
    std::string& error) noexcept {
    Close();
    try {
        destination_ = destination;
        partialPath_ = destination;
        partialPath_ += L".partial";
        if (destination_.has_parent_path()) std::filesystem::create_directories(destination_.parent_path());
        stream_.open(partialPath_, std::ios::binary | std::ios::trunc);
        if (!stream_) {
            error = "could not create the partial replay file";
            return false;
        }
        std::array<std::uint8_t, HeaderBytes> header{};
        std::copy(FileMagic.begin(), FileMagic.end(), header.begin());
        Put16(header, 8, FormatMajor);
        Put16(header, 10, FormatMinor);
        Put32(header, 12, HeaderBytes);
        Put64(header, 16, startedUnixMillis);
        if (!WriteAll(stream_, header)) {
            error = "could not write the replay header";
            Close();
            return false;
        }
        error.clear();
        return true;
    } catch (const std::exception& exception) {
        error = exception.what();
        Close();
        return false;
    } catch (...) {
        error = "unexpected replay writer failure";
        Close();
        return false;
    }
}

bool Writer::Append(RecordKind kind, std::uint64_t timestampMicros,
    std::span<const std::uint8_t> payload, std::string& error) noexcept {
    if (!stream_) {
        error = "replay writer is not open";
        return false;
    }
    if (payload.size() > MaxPayloadBytes || payload.size() > std::numeric_limits<std::uint32_t>::max()) {
        error = "replay record exceeds the payload limit";
        return false;
    }
    if (!WriteRecord(stream_, kind, timestampMicros, payload)) {
        error = "could not append a replay record";
        return false;
    }
    // Packet capture runs on a process we do not control. Keep each complete
    // record recoverable even if Minecraft or the injected DLL terminates
    // without reaching Finalize. A buffered writer can be added later behind
    // the same format once its bounded-loss policy is explicit.
    stream_.flush();
    if (!stream_) {
        error = "could not flush a replay record";
        return false;
    }
    error.clear();
    return true;
}

bool Writer::AppendMetadata(std::string_view json, std::string& error) noexcept {
    const auto* begin = reinterpret_cast<const std::uint8_t*>(json.data());
    return Append(RecordKind::Metadata, 0, {begin, json.size()}, error);
}

bool Writer::Finalize(std::uint64_t timestampMicros, std::string& error) noexcept {
    if (!Append(RecordKind::End, timestampMicros, {}, error)) return false;
    stream_.flush();
    if (!stream_) {
        error = "could not flush the replay file";
        Close();
        return false;
    }
    stream_.close();
    // The record stream is already flushed and closed above. REPLACE_EXISTING
    // keeps publication atomic on the same volume; WRITE_THROUGH is omitted
    // because packaged/AppContainer processes can reject that flag with
    // ERROR_ACCESS_DENIED even for an otherwise writable destination. Some
    // packaged processes may create and copy files in their brokered storage
    // but are denied a rename; Publish falls back to a copy there, and the End
    // record still lets readers distinguish a complete copied file.
    if (!Publish(partialPath_, destination_, error)) return false;
    error.clear();
    return true;
}

std::uint64_t Writer::BytesWritten() noexcept {
    if (!stream_.is_open()) return 0;
    const auto position = stream_.tellp();
    return position < 0 ? 0 : static_cast<std::uint64_t>(position);
}

bool ExportClip(const std::filesystem::path& source, std::uint64_t sourceBytes,
    const std::filesystem::path& destination, std::span<const Record> extra,
    std::uint64_t endTimestampMicros, std::string& error) noexcept {
    try {
        auto partial = destination;
        partial += L".partial";
        {
            std::ifstream input(source, std::ios::binary);
            std::ofstream output(partial, std::ios::binary | std::ios::trunc);
            if (!input || !output) {
                error = "could not open the replay clip files";
                return false;
            }
            std::vector<char> buffer(1U << 20U);
            std::uint64_t remaining = sourceBytes;
            while (remaining > 0) {
                const auto chunk = static_cast<std::streamsize>(std::min<std::uint64_t>(remaining, buffer.size()));
                input.read(buffer.data(), chunk);
                if (input.gcount() != chunk) {
                    error = "the recording is shorter than expected";
                    return false;
                }
                output.write(buffer.data(), chunk);
                remaining -= static_cast<std::uint64_t>(chunk);
            }
            for (const auto& record : extra) {
                if (record.payload.size() > MaxPayloadBytes
                    || !WriteRecord(output, record.kind, record.timestampMicros, record.payload)) {
                    error = "could not append a replay clip record";
                    return false;
                }
            }
            if (!WriteRecord(output, RecordKind::End, endTimestampMicros, {}) || !output.flush()) {
                error = "could not finish the replay clip";
                return false;
            }
        }
        if (!Publish(partial, destination, error)) return false;
        error.clear();
        return true;
    } catch (const std::exception& exception) {
        error = exception.what();
    } catch (...) {
        error = "unexpected replay clip failure";
    }
    return false;
}

void Writer::Close() noexcept {
    if (stream_.is_open()) stream_.close();
    destination_.clear();
    partialPath_.clear();
}

ReadResult Read(const std::filesystem::path& path) noexcept {
    ReadResult result;
    try {
        std::ifstream stream(path, std::ios::binary);
        if (!stream) {
            result.error = "could not open replay file";
            return result;
        }
        std::array<std::uint8_t, HeaderBytes> header{};
        stream.read(reinterpret_cast<char*>(header.data()), static_cast<std::streamsize>(header.size()));
        if (stream.gcount() != static_cast<std::streamsize>(header.size())) {
            result.error = "replay header is truncated";
            return result;
        }
        if (!std::equal(FileMagic.begin(), FileMagic.end(), header.begin())) {
            result.error = "replay magic is invalid";
            return result;
        }
        if (Get16(header, 8) != FormatMajor || Get32(header, 12) != HeaderBytes) {
            result.error = "replay format is not supported";
            return result;
        }
        result.startedUnixMillis = Get64(header, 16);
        for (;;) {
            std::array<std::uint8_t, RecordHeaderBytes> recordHeader{};
            stream.read(reinterpret_cast<char*>(recordHeader.data()), static_cast<std::streamsize>(recordHeader.size()));
            const auto headerRead = stream.gcount();
            if (headerRead == 0) return result; // valid crash-recovered prefix
            if (headerRead != static_cast<std::streamsize>(recordHeader.size())) return result;
            if (!std::equal(RecordMagic.begin(), RecordMagic.end(), recordHeader.begin())) {
                result.error = "replay record magic is invalid";
                return result;
            }
            const auto size = Get32(recordHeader, 16);
            if (size > MaxPayloadBytes) {
                result.error = "replay record exceeds the payload limit";
                return result;
            }
            Record record;
            record.kind = static_cast<RecordKind>(Get16(recordHeader, 4));
            record.timestampMicros = Get64(recordHeader, 8);
            record.payload.resize(size);
            stream.read(reinterpret_cast<char*>(record.payload.data()), static_cast<std::streamsize>(record.payload.size()));
            if (stream.gcount() != static_cast<std::streamsize>(record.payload.size())) return result;
            if (Checksum(record.payload) != Get32(recordHeader, 20)) {
                result.error = "replay record checksum is invalid";
                return result;
            }
            if (record.kind == RecordKind::End) {
                if (!record.payload.empty()) {
                    result.error = "replay end record is invalid";
                    return result;
                }
                result.cleanEnd = true;
                result.endTimestampMicros = record.timestampMicros;
                return result;
            }
            result.records.push_back(std::move(record));
        }
    } catch (const std::exception& exception) {
        result.error = exception.what();
    } catch (...) {
        result.error = "unexpected replay reader failure";
    }
    return result;
}

}
