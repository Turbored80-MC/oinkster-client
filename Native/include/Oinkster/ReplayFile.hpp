#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Oinkster::Replay {

inline constexpr std::uint16_t FormatMajor = 1;
inline constexpr std::uint16_t FormatMinor = 0;
inline constexpr std::uint32_t MaxPayloadBytes = 64U * 1024U * 1024U;

enum class RecordKind : std::uint16_t {
    Metadata = 1,
    IncomingPacket = 2,
    OutgoingPacket = 3,
    Camera = 4,
    Marker = 5,
    End = 0xffff,
};

struct Record {
    RecordKind kind{};
    std::uint64_t timestampMicros{};
    std::vector<std::uint8_t> payload;
};

struct ReadResult {
    std::uint64_t startedUnixMillis{};
    std::vector<Record> records;
    bool cleanEnd{};
    // Timestamp of the End record; zero unless cleanEnd.
    std::uint64_t endTimestampMicros{};
    std::string error;
};

// Append-only, checksummed replay container. Data is written to a .partial
// sibling and published to the requested .oinkreplay path after an End record
// reaches disk. Publication prefers an atomic rename; a partial file remains
// readable after a crash.
class Writer final {
public:
    Writer() = default;
    ~Writer();
    Writer(const Writer&) = delete;
    Writer& operator=(const Writer&) = delete;

    [[nodiscard]] bool Open(const std::filesystem::path& destination,
        std::uint64_t startedUnixMillis, std::string& error) noexcept;
    [[nodiscard]] bool Append(RecordKind kind, std::uint64_t timestampMicros,
        std::span<const std::uint8_t> payload, std::string& error) noexcept;
    [[nodiscard]] bool AppendMetadata(std::string_view json, std::string& error) noexcept;
    [[nodiscard]] bool Finalize(std::uint64_t timestampMicros, std::string& error) noexcept;
    void Close() noexcept;

    [[nodiscard]] bool IsOpen() const noexcept { return stream_.is_open(); }
    // Bytes committed so far (every Append is flushed), header included.
    [[nodiscard]] std::uint64_t BytesWritten() noexcept;
    [[nodiscard]] const std::filesystem::path& PartialPath() const noexcept { return partialPath_; }

private:
    std::filesystem::path destination_;
    std::filesystem::path partialPath_;
    std::ofstream stream_;
};

[[nodiscard]] ReadResult Read(const std::filesystem::path& path) noexcept;

// Publishes a clip of a recording that is still being written: the first
// sourceBytes of `source` (a whole number of records, e.g. from
// Writer::BytesWritten), then `extra` records, then an End record at
// endTimestampMicros. Written to a .partial sibling and renamed into place.
[[nodiscard]] bool ExportClip(const std::filesystem::path& source, std::uint64_t sourceBytes,
    const std::filesystem::path& destination, std::span<const Record> extra,
    std::uint64_t endTimestampMicros, std::string& error) noexcept;

}
