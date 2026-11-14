#include "Oinkster/ReplayPlayback.hpp"

#include "Oinkster/RakNet.hpp"
#include "Oinkster/ReplayCapture.hpp"
#include "Oinkster/ReplayTimeline.hpp"
#include "Oinkster/SupportedBuild.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <format>

namespace Oinkster::ReplayPlayback {
namespace {
namespace Id {
using namespace ReplayCapture::PacketId;
inline constexpr std::uint32_t AddPlayer = 12;
inline constexpr std::uint32_t MoveActorAbsolute = 18;
inline constexpr std::uint32_t MovePlayer = 19;
inline constexpr std::uint32_t ActorEvent = 27;
inline constexpr std::uint32_t MobEquipment = 31;
inline constexpr std::uint32_t MobArmorEquipment = 32;
inline constexpr std::uint32_t SetActorData = 39;
inline constexpr std::uint32_t Animate = 44;
inline constexpr std::uint32_t PlayerList = 63;
inline constexpr std::uint32_t SetActorMotion = 40;
inline constexpr std::uint32_t Respawn = 45;
inline constexpr std::uint32_t SetPlayerGameType = 62;
inline constexpr std::uint32_t ResourcePackDataInfo = 82;
inline constexpr std::uint32_t ResourcePackChunkData = 83;
inline constexpr std::uint32_t ResourcePackChunkRequest = 84;
inline constexpr std::uint32_t Transfer = 85;
inline constexpr std::uint32_t UpdatePlayerGameType = 151;
inline constexpr std::uint32_t CorrectPlayerMovePrediction = 161;
inline constexpr std::uint32_t UpdateAbilities = 187;
}

// ResourcePackClientResponse statuses for protocol 2193. These are varuints;
// the old 2/3/4 values left playback permanently waiting for completion.
constexpr std::uint32_t PacksSendPacks = 1;
constexpr std::uint32_t PacksHaveAll = 2;
constexpr std::uint32_t PacksCompleted = 3;
constexpr std::int32_t SpectatorGameType = 6;
constexpr std::int32_t LoginFailedClient = 1;
constexpr std::int32_t LoginFailedServer = 2;

std::int64_t ZigZagDecode(std::uint64_t value) noexcept {
    return static_cast<std::int64_t>(value >> 1U) ^ -static_cast<std::int64_t>(value & 1U);
}
std::uint32_t ZigZagEncode32(std::int32_t value) noexcept {
    return (static_cast<std::uint32_t>(value) << 1U) ^ static_cast<std::uint32_t>(value >> 31);
}

std::vector<std::uint8_t> PlayStatusPacket(std::int32_t value) {
    RakNet::Writer writer;
    writer.VarU32(ReplayCapture::PacketId::PlayStatus);
    writer.U32BE(static_cast<std::uint32_t>(value));
    return writer.Take();
}

std::span<const std::uint8_t> Body(std::span<const std::uint8_t> packet, const ReplayCapture::PacketHeader& header) {
    return packet.subspan(header.length);
}

std::optional<std::pair<std::string, std::uint32_t>> PackChunkKey(std::span<const std::uint8_t> body) {
    RakNet::Reader reader(body);
    const auto length = reader.VarU32();
    const auto name = reader.Bytes(length);
    const auto index = reader.Bytes(4);
    if (!reader.Ok()) return std::nullopt;
    const auto value = static_cast<std::uint32_t>(index[0])
        | (static_cast<std::uint32_t>(index[1]) << 8U)
        | (static_cast<std::uint32_t>(index[2]) << 16U)
        | (static_cast<std::uint32_t>(index[3]) << 24U);
    return std::pair{std::string(name.begin(), name.end()), value};
}

// The first field of these packets names the entity they move or control.
bool TargetsRuntimeId(std::span<const std::uint8_t> body, std::uint64_t runtimeId) {
    RakNet::Reader reader(body);
    const auto target = reader.VarU64();
    return reader.Ok() && target == runtimeId;
}

// The recording player: a server never spawns a client's own player for it,
// so in playback the person who recorded is re-created as a separate player
// (the "ghost") with their skin and name, walked along the recorded camera
// path. The viewer keeps the recorded StartGame identity, in spectator.
namespace Ghost {
// Far above the small, incrementing ids a server hands out.
constexpr std::uint64_t RuntimeId = 0x7FFF0001;
constexpr std::int64_t UniqueId = 0x7FFF0001;
constexpr float EyeHeight = 1.62F;
}

struct RecorderEntry {
    std::array<std::uint8_t, 16> uuid{};
    std::string name;
    // The unique id the list files the player under. Servers such as The
    // Hive give the joining client a different one in StartGame.
    std::int64_t uniqueId{};
    // Where in the PlayerList body the entry's UUID starts, and the length of
    // the UUID plus the unique-id varint that follows it.
    std::size_t offset{};
    std::size_t length{};
};

std::vector<std::uint8_t> ZigZagVarint64(std::int64_t value) {
    RakNet::Writer writer;
    writer.VarU64((static_cast<std::uint64_t>(value) << 1U) ^ static_cast<std::uint64_t>(value >> 63));
    return writer.Take();
}

void WriteString(RakNet::Writer& writer, std::string_view value) {
    writer.VarU32(static_cast<std::uint32_t>(value.size()));
    writer.Bytes(std::span(reinterpret_cast<const std::uint8_t*>(value.data()), value.size()));
}

void WriteU64LE(RakNet::Writer& writer, std::uint64_t value) {
    for (unsigned shift = 0; shift < 64; shift += 8) writer.U8(static_cast<std::uint8_t>(value >> shift));
}

// A player name as a list carries it: UTF-8 (servers colour names with
// section signs and add icon glyphs), no control characters.
bool PlausibleName(std::span<const std::uint8_t> name) {
    return !name.empty() && name.size() <= 128 && std::ranges::all_of(name, [](std::uint8_t c) { return c >= 0x20 && c != 0x7F; });
}

// On 1.26.5203 a list starts with its entry count, then 01 00 for an add or
// 00 01 for a removal (not the documented 00 (Add) and count): 01 01 00 for
// one player, 28 01 00 for forty. The length of that header, or nullopt.
std::optional<std::size_t> ListHeaderLength(std::span<const std::uint8_t> body) {
    RakNet::Reader reader(body);
    (void)reader.VarU32();
    const auto marker = reader.Bytes(2);
    if (!reader.Ok() || marker.size() != 2) return std::nullopt;
    const bool add = marker[0] == 0x01 && marker[1] == 0x00;
    const bool remove = marker[0] == 0x00 && marker[1] == 0x01;
    if (!add && !remove) return std::nullopt;
    return reader.Position();
}

// A PlayerList entry starts with its UUID and the player's unique id, then
// the name; skins make the rest of an entry impractical to walk, so the
// recorder's entry is found by its unique id followed by a plausible name.
std::optional<RecorderEntry> FindRecorderEntry(std::span<const std::uint8_t> body, std::int64_t uniqueId) {
    const auto id = ZigZagVarint64(uniqueId);
    for (std::size_t at = 16; at + id.size() < body.size(); ++at) {
        if (!std::equal(id.begin(), id.end(), body.begin() + static_cast<std::ptrdiff_t>(at))) continue;
        RakNet::Reader reader(body.subspan(at + id.size()));
        const auto nameLength = reader.VarU32();
        const auto name = reader.Bytes(nameLength);
        if (!reader.Ok() || !PlausibleName(name)) continue;
        RecorderEntry entry;
        std::copy_n(body.begin() + static_cast<std::ptrdiff_t>(at - 16), 16, entry.uuid.begin());
        entry.name.assign(name.begin(), name.end());
        entry.offset = at - 16;
        entry.length = 16 + id.size();
        entry.uniqueId = uniqueId;
        return entry;
    }
    return std::nullopt;
}

// The first entry of a list: a UUID right after the list header, the unique
// id, a name and a numeric XUID. Without a recognised header, a UUID a few
// bytes in - but the exact header comes first: a 40-entry list's 28 01 00
// can also parse one byte early (01 00 taken as the UUID's start, its last
// bytes as a long unique id) and re-keying that entry corrupts the header.
std::optional<RecorderEntry> FirstListEntry(std::span<const std::uint8_t> body) {
    const auto header = ListHeaderLength(body);
    std::vector<std::size_t> offsets;
    if (header) offsets.push_back(*header);
    for (std::size_t at = header.value_or(1); at <= 8; ++at) {
        if (!header || at != *header) offsets.push_back(at);
    }
    for (const auto at : offsets) {
        if (at + 16 >= body.size()) continue;
        RakNet::Reader reader(body.subspan(at + 16));
        const auto unique = reader.VarU64();
        const auto idLength = reader.Position();
        const auto name = reader.Bytes(reader.VarU32());
        const auto xuid = reader.Bytes(reader.VarU32());
        if (!reader.Ok() || !PlausibleName(name) || xuid.size() > 20
            || !std::ranges::all_of(xuid, [](std::uint8_t c) { return c >= '0' && c <= '9'; })) {
            continue;
        }
        RecorderEntry entry;
        std::copy_n(body.begin() + static_cast<std::ptrdiff_t>(at), 16, entry.uuid.begin());
        entry.name.assign(name.begin(), name.end());
        entry.offset = at;
        entry.length = 16 + idLength;
        entry.uniqueId = static_cast<std::int64_t>(unique >> 1U) ^ -static_cast<std::int64_t>(unique & 1U);
        return entry;
    }
    return std::nullopt;
}

std::array<std::uint8_t, 16> GhostUuid(const std::array<std::uint8_t, 16>& recorder) {
    auto uuid = recorder;
    for (std::size_t index = 0; index < 8; ++index) uuid[index] ^= 0xA5;
    return uuid;
}

// The recorder's entry re-keyed to the ghost; in any other list (a removal),
// every copy of the recorder's UUID is.
std::vector<std::uint8_t> RekeyPlayerList(std::span<const std::uint8_t> packet, const ReplayCapture::PacketHeader& header,
    const std::array<std::uint8_t, 16>& recorderUuid, std::int64_t recorderId) {
    const auto body = packet.subspan(header.length);
    const auto ghostUuid = GhostUuid(recorderUuid);
    const auto listHeader = ListHeaderLength(body).value_or(1);
    if (const auto entry = FindRecorderEntry(body, recorderId); entry && entry->uuid == recorderUuid) {
        // Never write over the list header: a mismatched entry there would
        // make the client reject the whole list (and leave the world).
        if (entry->offset < listHeader) return std::vector<std::uint8_t>(packet.begin(), packet.end());
        // The entry goes on: name, XUID, ... The XUID is cleared, as Playback
        // does, so no second player carries the viewer's own Xbox identity.
        RakNet::Reader reader(body.subspan(entry->offset + entry->length));
        const auto name = reader.Bytes(reader.VarU32());
        (void)reader.Bytes(reader.VarU32());  // XUID
        if (!reader.Ok()) return std::vector<std::uint8_t>(packet.begin(), packet.end());
        RakNet::Writer writer;
        writer.Bytes(packet.subspan(0, header.length + entry->offset));
        writer.Bytes(ghostUuid);
        writer.Bytes(ZigZagVarint64(Ghost::UniqueId));
        writer.VarU32(static_cast<std::uint32_t>(name.size()));
        writer.Bytes(name);
        writer.VarU32(0);
        writer.Bytes(reader.Rest());
        return writer.Take();
    }
    std::vector<std::uint8_t> copy(packet.begin(), packet.end());
    for (std::size_t at = header.length + listHeader; at + 16 <= copy.size(); ++at) {
        if (std::equal(recorderUuid.begin(), recorderUuid.end(), copy.begin() + static_cast<std::ptrdiff_t>(at))) {
            std::copy(ghostUuid.begin(), ghostUuid.end(), copy.begin() + static_cast<std::ptrdiff_t>(at));
        }
    }
    return copy;
}

// Swings, held items, armor, entity events and data the server addressed to
// the recording player belong to the ghost now. nullopt: not one of these.
std::optional<std::vector<std::uint8_t>> RetargetToGhost(std::span<const std::uint8_t> packet,
    const ReplayCapture::PacketHeader& header, std::uint64_t recorderRuntimeId) {
    switch (header.id) {
    case Id::ActorEvent: case Id::MobEquipment: case Id::MobArmorEquipment: case Id::SetActorData: case Id::Animate:
        break;
    default:
        return std::nullopt;
    }
    RakNet::Reader reader(packet.subspan(header.length));
    if (header.id == Id::Animate) (void)reader.VarU32();
    const auto targetStart = header.length + reader.Position();
    const auto target = reader.VarU64();
    if (!reader.Ok() || target != recorderRuntimeId) return std::nullopt;
    RakNet::Writer writer;
    writer.Bytes(packet.subspan(0, targetStart));
    writer.VarU64(Ghost::RuntimeId);
    writer.Bytes(packet.subspan(header.length + reader.Position()));
    return writer.Take();
}

std::vector<std::uint8_t> GhostAddPlayer(const std::array<std::uint8_t, 16>& uuid, std::string_view name,
    const Replay::CameraSample& at) {
    RakNet::Writer writer;
    writer.VarU32(Id::AddPlayer);
    writer.Bytes(uuid);
    WriteString(writer, name);
    writer.VarU64(Ghost::RuntimeId);
    WriteString(writer, "");                      // platform chat id
    writer.F32LE(static_cast<float>(at.x));       // feet position
    writer.F32LE(static_cast<float>(at.y));
    writer.F32LE(static_cast<float>(at.z));
    writer.F32LE(0.0F);                           // velocity
    writer.F32LE(0.0F);
    writer.F32LE(0.0F);
    writer.F32LE(at.pitch.value_or(0.0F));
    writer.F32LE(at.yaw.value_or(0.0F));
    writer.F32LE(at.yaw.value_or(0.0F));          // head yaw
    // Held item, as 1.26.50 (protocol 2193) writes an ItemInstance: no
    // short form for air any more - int16 id, u16 count, varuint metadata,
    // has-stack-id bool, varuint block runtime id, then user data (a
    // varuint length, 0 when there is no item).
    writer.U16LE(0);                              // id: air
    writer.U16LE(0);                              // count
    writer.VarU32(0);                             // metadata
    writer.U8(0);                                 // no stack network id
    writer.VarU32(0);                             // block runtime id
    writer.VarU32(0);                             // user data
    writer.VarU32(ZigZagEncode32(0));             // game type: survival
    writer.VarU32(0);                             // actor metadata
    writer.VarU32(0);                             // int properties
    writer.VarU32(0);                             // float properties
    WriteU64LE(writer, static_cast<std::uint64_t>(Ghost::UniqueId));  // abilities: unique id
    writer.U8(1);                                 // player permissions: member
    writer.U8(0);                                 // command permissions
    writer.U8(0);                                 // ability layers
    writer.VarU32(0);                             // actor links
    WriteString(writer, "");                      // device id
    writer.U32LE(0xFFFFFFFFU);                    // build platform: unknown
    return writer.Take();
}

// An angle as MoveActor packets carry it: 1/256ths of a full turn.
std::uint8_t AngleByte(float degrees) {
    const auto turns = std::lround(static_cast<double>(degrees) * 256.0 / 360.0);
    return static_cast<std::uint8_t>(turns & 0xFF);
}

// The ghost walks the recorded camera path the way servers move other
// players: MoveActorAbsolute, whose head yaw and pitch the client turns the
// model's head with (MovePlayer for a remote player left its head level).
std::vector<std::uint8_t> GhostMoveActor(const Replay::CameraSample& at) {
    RakNet::Writer writer;
    writer.VarU32(Id::MoveActorAbsolute);
    writer.VarU64(Ghost::RuntimeId);
    writer.U8(0x01);                              // flags: on ground
    writer.F32LE(static_cast<float>(at.x));       // players move at eye height
    writer.F32LE(static_cast<float>(at.y) + Ghost::EyeHeight);
    writer.F32LE(static_cast<float>(at.z));
    writer.U8(AngleByte(at.pitch.value_or(0.0F)));
    writer.U8(AngleByte(at.yaw.value_or(0.0F)));  // body yaw
    writer.U8(AngleByte(at.yaw.value_or(0.0F)));  // head yaw
    return writer.Take();
}

// --- Old-height server chunks ----------------------------------------------
// Some servers (The Hive's lobbies among them) still run worlds from before
// 1.18: 0-255 tall, sent as full LevelChunks of version-8 sub-chunks, which
// carry no height, plus exactly 16 biome sections. Live, the client takes
// that world's height from the server. The replay world is a modern one
// (-64 to 320, 24 biome sections), so the same chunk no longer lines up and
// none of its blocks appear. Each such chunk is rewritten into the modern
// layout: version-9 sub-chunks tagged with their absolute height, and the
// biomes padded out to 24 sections (below 0 copies the lowest section, above
// 255 repeats the highest). Anything that does not parse is left as it was.
namespace OldHeight {
constexpr std::size_t OldSections = 16;
constexpr std::size_t ModernSections = 24;
constexpr std::size_t SectionsBelowZero = 4;
constexpr std::uint8_t SameAsBelow = 0xFF;
constexpr std::array<unsigned, 8> PaletteBits{1, 2, 3, 4, 5, 6, 8, 16};

std::int64_t SignedCount(RakNet::Reader& reader) { return ZigZagDecode(reader.VarU32()); }

// Skips one network-NBT value (varint lengths and ints) of type `tag`.
bool SkipNbt(RakNet::Reader& reader, std::uint8_t tag, int depth) {
    if (depth > 16) return false;
    switch (tag) {
    case 1: reader.Skip(1); break;
    case 2: reader.Skip(2); break;
    case 3: (void)reader.VarU32(); break;
    case 4: (void)reader.VarU64(); break;
    case 5: reader.Skip(4); break;
    case 6: reader.Skip(8); break;
    case 7: {
        const auto length = SignedCount(reader);
        if (length < 0) return false;
        reader.Skip(static_cast<std::size_t>(length));
        break;
    }
    case 8: reader.Skip(reader.VarU32()); break;
    case 9: {
        const auto element = reader.U8();
        const auto length = SignedCount(reader);
        if (length < 0 || length > 65536) return false;
        for (std::int64_t index = 0; index < length && reader.Ok(); ++index) {
            if (!SkipNbt(reader, element, depth + 1)) return false;
        }
        break;
    }
    case 10:
        for (;;) {
            const auto inner = reader.U8();
            if (!reader.Ok()) return false;
            if (inner == 0) break;
            reader.Skip(reader.VarU32());
            if (!SkipNbt(reader, inner, depth + 1)) return false;
        }
        break;
    case 11: case 12: {
        const auto length = SignedCount(reader);
        if (length < 0 || length > 65536) return false;
        for (std::int64_t index = 0; index < length && reader.Ok(); ++index) (void)reader.VarU64();
        break;
    }
    default:
        return false;
    }
    return reader.Ok();
}

// Skips a paletted storage whose header byte was already read. Block
// palettes are runtime ids or (the low header bit clear) named NBT states;
// biome palettes are always ids.
bool SkipStorage(RakNet::Reader& reader, std::uint8_t header, bool blocks) {
    const unsigned bits = header >> 1U;
    const bool runtime = (header & 1U) != 0;
    if (bits == 0) {
        if (blocks && !runtime) return false;
        (void)reader.VarU32();
        return reader.Ok();
    }
    if (std::ranges::find(PaletteBits, bits) == PaletteBits.end()) return false;
    const std::size_t perWord = 32U / bits;
    reader.Skip((4096U + perWord - 1U) / perWord * 4U);
    const auto entries = SignedCount(reader);
    if (!reader.Ok() || entries <= 0 || entries > 4096) return false;
    for (std::int64_t index = 0; index < entries && reader.Ok(); ++index) {
        if (!blocks || runtime) {
            (void)reader.VarU32();
            continue;
        }
        const auto tag = reader.U8();
        reader.Skip(reader.VarU32());  // root name
        if (!SkipNbt(reader, tag, 0)) return false;
    }
    return reader.Ok();
}

// The chunk in the modern layout, or nullopt when it is not an old-height
// full chunk (request-mode and cached chunks already say where they go).
std::optional<std::vector<std::uint8_t>> Modernize(std::span<const std::uint8_t> packet, const ReplayCapture::PacketHeader& header) {
    const auto body = packet.subspan(header.length);
    RakNet::Reader reader(body);
    (void)reader.VarU32();  // x
    (void)reader.VarU32();  // z
    (void)reader.VarU32();  // dimension
    const auto sections = reader.VarU32();
    const auto requestMode = reader.U8();
    const auto cached = reader.U8();
    (void)reader.U8();
    const auto lengthAt = reader.Position();
    const auto length = reader.VarU32();
    const auto payloadAt = reader.Position();
    if (!reader.Ok() || requestMode != 0 || cached != 0 || sections == 0 || sections > OldSections
        || payloadAt + length != body.size()) {
        return std::nullopt;
    }
    const auto payload = body.subspan(payloadAt);
    RakNet::Reader chunk(payload);
    RakNet::Writer rewritten;
    for (std::uint32_t index = 0; index < sections; ++index) {
        const auto version = chunk.U8();
        const auto layers = chunk.U8();
        if (!chunk.Ok() || version != 8 || layers == 0 || layers > 4) return std::nullopt;
        const auto start = chunk.Position();
        for (std::uint8_t layer = 0; layer < layers; ++layer) {
            if (!SkipStorage(chunk, chunk.U8(), true)) return std::nullopt;
        }
        rewritten.U8(9);
        rewritten.U8(layers);
        rewritten.U8(static_cast<std::uint8_t>(index));  // absolute sub-chunk y
        rewritten.Bytes(payload.subspan(start, chunk.Position() - start));
    }
    const auto biomesAt = chunk.Position();
    std::size_t lowestEnd{};
    for (std::size_t section = 0; section < OldSections; ++section) {
        const auto marker = chunk.U8();
        if (!chunk.Ok() || (marker == SameAsBelow && section == 0)) return std::nullopt;
        if (marker != SameAsBelow && !SkipStorage(chunk, marker, false)) return std::nullopt;
        if (section == 0) lowestEnd = chunk.Position();
    }
    // Then the border-block count (none) and any block entities: compounds.
    // Anything else means this chunk does not have 16 biome sections.
    const auto rest = chunk.Rest();
    if (rest.empty() || rest[0] != 0 || (rest.size() > 1 && rest[1] != 10)) return std::nullopt;
    rewritten.Bytes(payload.subspan(biomesAt, lowestEnd - biomesAt));
    for (std::size_t section = 1; section < SectionsBelowZero; ++section) rewritten.U8(SameAsBelow);
    rewritten.Bytes(payload.subspan(biomesAt, chunk.Position() - biomesAt));
    for (std::size_t section = SectionsBelowZero + OldSections; section < ModernSections; ++section) rewritten.U8(SameAsBelow);
    rewritten.Bytes(rest);
    const auto newPayload = rewritten.Take();
    RakNet::Writer writer;
    writer.Bytes(packet.subspan(0, header.length + lengthAt));
    writer.VarU32(static_cast<std::uint32_t>(newPayload.size()));
    writer.Bytes(newPayload);
    return writer.Take();
}
}

// --- Recording player's equipment -----------------------------------------
// A server never sends a client its own MobEquipment, so the ghost's hand and
// armor are rebuilt from what the recording does hold: the player's
// inventory (InventoryContent / InventorySlot, window 0 = inventory, 120 =
// armor) and the selected hotbar slot (camera samples since it was
// recorded; the server's rare PlayerHotBar otherwise).
namespace Equip {
inline constexpr std::uint32_t PlayerHotBar = 48;
inline constexpr std::uint32_t InventoryContent = 49;
inline constexpr std::uint32_t InventorySlot = 50;
inline constexpr std::uint32_t InventoryWindow = 0;
inline constexpr std::uint32_t ArmorWindow = 120;
inline constexpr std::size_t InventorySlots = 36;
inline constexpr std::size_t ArmorSlots = 5;  // helmet, chest, legs, boots, body
}

// An empty 1.26.50 ItemInstance (see GhostAddPlayer).
const std::vector<std::uint8_t>& EmptyItem() {
    static const std::vector<std::uint8_t> empty(8, 0);
    return empty;
}

// One 1.26.50 ItemInstance at the reader's position: int16 id, u16 count,
// varuint metadata, bool has-stack-id (+ varint id), varuint block runtime
// id, varuint-length user data. nullopt when it does not fit.
std::optional<std::vector<std::uint8_t>> ReadItemInstance(RakNet::Reader& reader) {
    const auto start = reader.Rest();
    const auto before = reader.Remaining();
    reader.Skip(2 + 2);
    (void)reader.VarU32();
    if (reader.U8() != 0) (void)reader.VarU32();
    (void)reader.VarU32();
    reader.Skip(reader.VarU32());
    if (!reader.Ok()) return std::nullopt;
    const auto length = before - reader.Remaining();
    return std::vector<std::uint8_t>(start.begin(), start.begin() + static_cast<std::ptrdiff_t>(length));
}

struct EquipmentChange {
    std::uint64_t t{};
    std::uint32_t window{};
    // Whole-window content, or one slot.
    std::optional<std::vector<std::vector<std::uint8_t>>> content;
    std::uint32_t slot{};
    std::vector<std::uint8_t> item;
    std::optional<int> selected;
    // A selection from the client's own samples rather than the server.
    bool sampled{};
};

std::optional<EquipmentChange> ParseEquipmentChange(std::uint64_t t, std::span<const std::uint8_t> packet) {
    const auto header = ReplayCapture::ParseHeader(packet);
    if (!header) return std::nullopt;
    RakNet::Reader reader(packet.subspan(header->length));
    EquipmentChange change{t};
    if (header->id == Equip::PlayerHotBar) {
        const auto selected = reader.VarU32();
        if (!reader.Ok() || selected > 8) return std::nullopt;
        change.selected = static_cast<int>(selected);
        return change;
    }
    if (header->id != Equip::InventoryContent && header->id != Equip::InventorySlot) return std::nullopt;
    change.window = reader.VarU32();
    if (change.window != Equip::InventoryWindow && change.window != Equip::ArmorWindow) return std::nullopt;
    if (header->id == Equip::InventoryContent) {
        const auto count = reader.VarU32();
        if (!reader.Ok() || count > 64) return std::nullopt;
        std::vector<std::vector<std::uint8_t>> items;
        for (std::uint32_t index = 0; index < count; ++index) {
            auto item = ReadItemInstance(reader);
            if (!item) return std::nullopt;
            items.push_back(std::move(*item));
        }
        change.content = std::move(items);
        return change;
    }
    change.slot = reader.VarU32();
    if (reader.U8() != 0) {  // container name: u8 id + optional u32
        (void)reader.U8();
        if (reader.U8() != 0) reader.Skip(4);
    }
    if (reader.U8() != 0 && !ReadItemInstance(reader)) return std::nullopt;  // storage item
    auto item = ReadItemInstance(reader);
    if (!item || !reader.Ok()) return std::nullopt;
    change.item = std::move(*item);
    return change;
}

std::vector<std::uint8_t> GhostMobEquipment(const std::vector<std::uint8_t>& item, int slot) {
    RakNet::Writer writer;
    writer.VarU32(Id::MobEquipment);
    writer.VarU64(Ghost::RuntimeId);
    writer.Bytes(item);
    writer.U8(static_cast<std::uint8_t>(slot));  // inventory slot
    writer.U8(static_cast<std::uint8_t>(slot));  // hotbar slot
    writer.U8(0);                                // window: inventory
    return writer.Take();
}

std::vector<std::uint8_t> GhostArmor(const std::vector<std::vector<std::uint8_t>>& armor) {
    RakNet::Writer writer;
    writer.VarU32(Id::MobArmorEquipment);
    writer.VarU64(Ghost::RuntimeId);
    for (std::size_t index = 0; index < Equip::ArmorSlots; ++index) {
        writer.Bytes(index < armor.size() && !armor[index].empty() ? armor[index] : EmptyItem());
    }
    return writer.Take();
}

// Packets that would take control of the viewer's camera or send them away.
bool DropDuringPlayback(std::uint32_t id, std::span<const std::uint8_t> body, const StartGameIds& ids) {
    switch (id) {
    case Id::NetworkSettings: case Id::ServerToClientHandshake: case Id::ResourcePacksInfo: case Id::ResourcePackStack:
    case Id::ResourcePackDataInfo: case Id::ResourcePackChunkData: case Id::Disconnect: case Id::Transfer:
    case Id::SetPlayerGameType: case Id::UpdatePlayerGameType: case Id::CorrectPlayerMovePrediction:
        return true;
    case Id::MovePlayer: case Id::SetActorMotion:
        return TargetsRuntimeId(body, ids.runtimeId);
    case Id::Respawn: {
        RakNet::Reader reader(body);
        reader.Skip(12 + 1);
        const auto target = reader.VarU64();
        return reader.Ok() && target == ids.runtimeId;
    }
    case Id::UpdateAbilities: {
        if (body.size() < 8) return false;
        std::int64_t target{};
        for (int index = 7; index >= 0; --index) target = (target << 8) | body[static_cast<std::size_t>(index)];
        return target == ids.uniqueId;
    }
    default:
        return false;
    }
}
}

std::vector<std::uint8_t> EncodeBatch(std::span<const std::vector<std::uint8_t>> packets, bool compressionHeader) {
    RakNet::Writer writer;
    writer.U8(RakNet::MessageId::GamePacket);
    if (compressionHeader) writer.U8(0xFF);
    for (const auto& packet : packets) {
        writer.VarU32(static_cast<std::uint32_t>(packet.size()));
        writer.Bytes(packet);
    }
    return writer.Take();
}

std::optional<std::vector<std::vector<std::uint8_t>>> DecodeBatch(std::span<const std::uint8_t> message, bool compressionHeader) {
    RakNet::Reader reader(message);
    if (reader.U8() != RakNet::MessageId::GamePacket) return std::nullopt;
    if (compressionHeader && reader.U8() != 0xFF) return std::nullopt;
    std::vector<std::vector<std::uint8_t>> packets;
    while (reader.Ok() && reader.Remaining() > 0 && packets.size() < 4096) {
        const auto length = reader.VarU32();
        const auto bytes = reader.Bytes(length);
        if (!reader.Ok()) return std::nullopt;
        packets.emplace_back(bytes.begin(), bytes.end());
    }
    if (!reader.Ok()) return std::nullopt;
    return packets;
}

std::vector<std::uint8_t> NetworkSettingsPacket() {
    RakNet::Writer writer;
    writer.VarU32(ReplayCapture::PacketId::NetworkSettings);
    writer.U16LE(0xFFFF);  // compression threshold
    writer.U16LE(0);       // zlib
    writer.U8(0);          // client throttle off
    writer.U8(0);
    writer.F32LE(0.0F);
    return writer.Take();
}

std::optional<std::vector<std::uint8_t>> PatchStartGameToSpectator(std::span<const std::uint8_t> packet, StartGameIds& ids) {
    const auto header = ReplayCapture::ParseHeader(packet);
    if (!header || header->id != ReplayCapture::PacketId::StartGame) return std::nullopt;
    RakNet::Reader reader(Body(packet, *header));
    ids.uniqueId = ZigZagDecode(reader.VarU64());
    ids.runtimeId = reader.VarU64();
    const auto gameModeStart = header->length + reader.Position();
    (void)reader.VarU32();
    if (!reader.Ok()) return std::nullopt;
    const auto gameModeEnd = header->length + reader.Position();
    RakNet::Writer writer;
    writer.Bytes(packet.subspan(0, gameModeStart));
    writer.VarU32(ZigZagEncode32(SpectatorGameType));
    writer.Bytes(packet.subspan(gameModeEnd));
    return writer.Take();
}

std::optional<Session> Session::Load(const Replay::ReadResult& replay, std::string& error) {
    Session session;
    const auto playbackClock = Replay::BuildPlaybackClock(replay);
    session.clipStart_ = playbackClock.clipStartMicros;
    session.endMicros_ = playbackClock.endMicros;
    StartGameIds ids;
    bool sawStartGame = false;
    bool afterStack = false;
    std::uint64_t startGameMicros{};
    // The recording player's ids (from StartGame) and PlayerList entry are
    // needed before StartGame itself: the list is sent first.
    StartGameIds recorderIds;
    std::optional<RecorderEntry> recorder;
    for (const auto& record : replay.records) {
        if (record.kind != Replay::RecordKind::IncomingPacket) continue;
        const auto header = ReplayCapture::ParseHeader(record.payload);
        if (header && header->id == Id::StartGame && PatchStartGameToSpectator(record.payload, recorderIds)) break;
    }
    for (const auto& record : replay.records) {
        if (record.kind != Replay::RecordKind::IncomingPacket) continue;
        const auto header = ReplayCapture::ParseHeader(record.payload);
        if (!header || header->id != Id::PlayerList) continue;
        if ((recorder = FindRecorderEntry(Body(record.payload, *header), recorderIds.uniqueId))) break;
    }
    if (!recorder) {
        // No entry under the StartGame id (The Hive lists the joining player
        // under another one). A server never spawns a client's own player,
        // so the recorder is the first list's first entry if nobody with that
        // UUID is ever spawned with AddPlayer.
        std::vector<std::array<std::uint8_t, 16>> spawned;
        for (const auto& record : replay.records) {
            if (record.kind != Replay::RecordKind::IncomingPacket) continue;
            const auto header = ReplayCapture::ParseHeader(record.payload);
            const auto body = header ? Body(record.payload, *header) : std::span<const std::uint8_t>{};
            if (!header || header->id != Id::AddPlayer || body.size() < 16) continue;
            std::array<std::uint8_t, 16> uuid{};
            std::copy_n(body.begin(), 16, uuid.begin());
            spawned.push_back(uuid);
        }
        for (const auto& record : replay.records) {
            if (record.kind != Replay::RecordKind::IncomingPacket) continue;
            const auto header = ReplayCapture::ParseHeader(record.payload);
            if (!header || header->id != Id::PlayerList) continue;
            const auto first = FirstListEntry(Body(record.payload, *header));
            if (first && std::ranges::find(spawned, first->uuid) == spawned.end()) recorder = first;
            break;
        }
    }
    for (const auto& record : replay.records) {
        if (record.kind == Replay::RecordKind::Marker) {
            continue;
        }
        if (record.kind != Replay::RecordKind::IncomingPacket) continue;
        const auto header = ReplayCapture::ParseHeader(record.payload);
        if (!header) continue;
        if (header->id == Id::ResourcePacksInfo && session.packsInfo_.empty()) {
            session.packsInfo_ = record.payload;
            continue;
        }
        if (header->id == Id::ResourcePackDataInfo) {
            session.packDataInfo_.push_back(record.payload);
            continue;
        }
        if (header->id == Id::ResourcePackChunkData) {
            if (auto key = PackChunkKey(Body(record.payload, *header))) {
                session.packChunks_.push_back({std::move(key->first), key->second, record.payload});
            }
            continue;
        }
        if (header->id == Id::ResourcePackStack && session.packStack_.empty()) {
            session.packStack_ = record.payload;
            afterStack = true;
            continue;
        }
        // Everything before the pack stack is handshake we answer ourselves.
        if (!afterStack) continue;
        auto bytes = record.payload;
        if (header->id == Id::StartGame && !sawStartGame) {
            auto patched = PatchStartGameToSpectator(record.payload, ids);
            if (!patched) {
                error = "The recorded StartGame packet could not be read.";
                return std::nullopt;
            }
            bytes = std::move(*patched);
            sawStartGame = true;
            session.customBlocks_ = ReplayBlockPack::FindCustomBlocks(record.payload);
            startGameMicros = playbackClock.Map(record.timestampMicros);
        } else if (DropDuringPlayback(header->id, Body(record.payload, *header), ids)) {
            continue;
        } else if (header->id == Id::LevelChunk) {
            if (auto modern = OldHeight::Modernize(record.payload, *header)) {
                bytes = std::move(*modern);
                ++session.modernizedChunks_;
            }
        } else if (header->id == Id::PlayerList && recorder) {
            bytes = RekeyPlayerList(record.payload, *header, recorder->uuid, recorder->uniqueId);
        } else if (auto retargeted = RetargetToGhost(record.payload, *header, recorderIds.runtimeId)) {
            bytes = std::move(*retargeted);
        }
        session.stream_.push_back({playbackClock.Map(record.timestampMicros), header->id, std::move(bytes)});
    }
    if (sawStartGame) session.AddGhost(replay, recorder ? recorder->uuid : std::array<std::uint8_t, 16>{},
        recorder ? recorder->name : std::string("Replay"), startGameMicros);
    if (session.packsInfo_.empty() || session.packStack_.empty() || !sawStartGame) {
        error = "This replay has no world data to play back.";
        return std::nullopt;
    }
    session.clipStart_ = std::min(session.clipStart_, session.endMicros_);
    session.viewerRuntimeId_ = ids.runtimeId;
    session.IndexForSeeking(replay);
    return session;
}

void Session::IndexForSeeking(const Replay::ReadResult& replay) {
    constexpr std::uint32_t PlayerSpawn = 3;
    for (std::size_t index = 0; index < stream_.size(); ++index) {
        const auto& packet = stream_[index];
        if (packet.id == Id::StartGame && afterStartGame_ == 0) afterStartGame_ = index + 1;
        if (afterStartGame_ != 0 && spawnIndex_ == 0 && packet.id == ReplayCapture::PacketId::PlayStatus) {
            const auto header = ReplayCapture::ParseHeader(packet.bytes);
            RakNet::Reader reader(std::span(packet.bytes).subspan(header ? header->length : 0));
            if (header && reader.U32BE() == PlayerSpawn && reader.Ok()) spawnIndex_ = index;
        }
    }
    if (spawnIndex_ == 0) spawnIndex_ = afterStartGame_;

    // Every block the recording changes (UpdateBlock and UpdateSubChunkBlocks'
    // standard and extra lists), first change only.
    constexpr std::uint32_t UpdateBlock = 21, UpdateSubChunkBlocks = 172;
    const auto touch = [&](std::uint32_t x, std::uint32_t y, std::uint32_t z, std::uint32_t runtimeId,
        std::uint32_t layer, std::uint64_t t) {
        const auto found = std::ranges::find_if(blockTouches_, [&](const BlockTouch& block) {
            return block.x == x && block.y == y && block.z == z && block.layer == layer;
        });
        if (found == blockTouches_.end()) blockTouches_.push_back({x, y, z, layer, runtimeId, t});
    };
    bool sawSmallId = false;
    for (const auto& packet : stream_) {
        if (packet.id != UpdateBlock && packet.id != UpdateSubChunkBlocks) continue;
        const auto header = ReplayCapture::ParseHeader(packet.bytes);
        if (!header) continue;
        RakNet::Reader reader(std::span(packet.bytes).subspan(header->length));
        if (packet.id == UpdateBlock) {
            const auto x = reader.VarU32(), y = reader.VarU32(), z = reader.VarU32();
            const auto runtimeId = reader.VarU32();
            (void)reader.VarU32();  // flags
            const auto layer = reader.VarU32();
            if (!reader.Ok()) continue;
            sawSmallId |= runtimeId < 0x10000U;
            touch(x, y, z, runtimeId, layer, packet.t);
            continue;
        }
        (void)reader.VarU32(); (void)reader.VarU32(); (void)reader.VarU32();  // sub-chunk position
        for (std::uint32_t layer = 0; layer < 2 && reader.Ok(); ++layer) {
            const auto count = reader.VarU32();
            for (std::uint32_t index = 0; index < count && index < 4096 && reader.Ok(); ++index) {
                const auto x = reader.VarU32(), y = reader.VarU32(), z = reader.VarU32();
                const auto runtimeId = reader.VarU32();
                (void)reader.VarU32();  // flags
                (void)reader.VarU64();  // synced update actor
                (void)reader.VarU32();  // synced update type
                if (!reader.Ok()) break;
                sawSmallId |= runtimeId < 0x10000U;
                touch(x, y, z, runtimeId, layer, packet.t);
            }
        }
    }
    // Palette indices are small; hashes almost never are.
    blockIdsHashed_ = !blockTouches_.empty() && !sawSmallId;
    const auto playbackClock = Replay::BuildPlaybackClock(replay);
    for (const auto& record : replay.records) {
        if (record.kind != Replay::RecordKind::Marker) continue;
        std::string label(reinterpret_cast<const char*>(record.payload.data()), record.payload.size());
        if (label == "clip-start" || label == "pause" || label == "resume") continue;
        const auto at = playbackClock.Map(record.timestampMicros);
        if (at < clipStart_) continue;
        marks_.push_back({at - clipStart_, std::move(label)});
    }
}

void Session::AddGhost(const Replay::ReadResult& replay, const std::array<std::uint8_t, 16>& recorderUuid,
    const std::string& name, std::uint64_t startGameMicros) {
    const auto timeline = Replay::BuildTimeline(replay);
    if (timeline.samples.empty()) return;
    // Spawned where the recording player stood when the clip begins; walked
    // from there, so catch-up does not replay the whole path before it.
    const auto spawnAt = std::max(startGameMicros, clipStart_);
    const auto spawn = Replay::SampleAt(timeline, spawnAt);
    if (!spawn) return;
    stream_.push_back({spawnAt, Id::AddPlayer, GhostAddPlayer(GhostUuid(recorderUuid), name, *spawn), true});
    ghost_ = true;
    ghostUuid_ = GhostUuid(recorderUuid);
    ghostName_ = name;
    ghostSkin_ = recorderUuid != std::array<std::uint8_t, 16>{};
    camera_.samples = timeline.samples;
    for (const auto& sample : timeline.samples) {
        if (sample.t <= spawnAt) continue;
        stream_.push_back({sample.t, Id::MoveActorAbsolute, GhostMoveActor(sample)});
    }
    AddGhostEquipment(replay, timeline, spawnAt);
    // Recorded packets keep their order; ghost packets slot in by time, after
    // any recorded packet with the same timestamp.
    std::stable_sort(stream_.begin(), stream_.end(), [](const Packet& left, const Packet& right) { return left.t < right.t; });
}

bool Session::AnswerPackDownload(std::span<const std::uint8_t> packet, std::vector<std::vector<std::uint8_t>>& out) const {
    const auto header = ReplayCapture::ParseHeader(packet);
    if (!header || packDataInfo_.empty()) return false;
    const auto body = Body(packet, *header);
    if (header->id == Id::ResourcePackClientResponse) {
        RakNet::Reader reader(body);
        if (reader.VarU32() != PacksSendPacks || !reader.Ok()) return false;
        out.insert(out.end(), packDataInfo_.begin(), packDataInfo_.end());
        return true;
    }
    if (header->id != Id::ResourcePackChunkRequest) return false;
    const auto requested = PackChunkKey(body);
    if (!requested) return false;
    const auto found = std::ranges::find_if(packChunks_, [&](const PackChunk& chunk) {
        return chunk.uuid == requested->first && chunk.index == requested->second;
    });
    if (found == packChunks_.end()) return false;
    out.push_back(found->bytes);
    return true;
}

std::vector<std::uint8_t> Session::GhostRemovePacket() const {
    if (!ghost_) return {};
    constexpr std::uint32_t RemoveActor = 14;
    RakNet::Writer writer;
    writer.VarU32(RemoveActor);
    writer.Bytes(ZigZagVarint64(Ghost::UniqueId));
    return writer.Take();
}

std::vector<std::uint8_t> Session::GhostSpawnPacket(double x, double eyeY, double z, float yaw, float pitch) const {
    if (!ghost_) return {};
    Replay::CameraSample at;
    at.x = x;
    at.y = eyeY - Ghost::EyeHeight;
    at.z = z;
    at.yaw = yaw;
    at.pitch = pitch;
    return GhostAddPlayer(ghostUuid_, ghostName_, at);
}

std::optional<Replay::CameraSample> Session::RecorderCameraAt(Clock::time_point now) const {
    if (!ghost_ || camera_.samples.empty()) return std::nullopt;
    const auto absolute = AbsoluteMicros(now);
    return Replay::SampleAt(camera_, absolute > 0.0 ? static_cast<std::uint64_t>(absolute) : 0);
}

void Session::AddGhostEquipment(const Replay::ReadResult& replay, const Replay::ReplayTimeline& timeline,
    std::uint64_t spawnAt) {
    std::vector<EquipmentChange> changes;
    const auto playbackClock = Replay::BuildPlaybackClock(replay);
    for (const auto& record : replay.records) {
        if (record.kind != Replay::RecordKind::IncomingPacket) continue;
        if (auto change = ParseEquipmentChange(playbackClock.Map(record.timestampMicros), record.payload)) {
            changes.push_back(std::move(*change));
        }
    }
    bool sampledSlots = false;
    for (const auto& sample : timeline.samples) {
        if (!sample.hotbarSlot) continue;
        sampledSlots = true;
        EquipmentChange change{sample.t};
        change.selected = sample.hotbarSlot;
        change.sampled = true;
        changes.push_back(std::move(change));
    }
    std::ranges::stable_sort(changes, {}, &EquipmentChange::t);

    std::vector<std::vector<std::uint8_t>> inventory(Equip::InventorySlots);
    std::vector<std::vector<std::uint8_t>> armor(Equip::ArmorSlots);
    int selected = 0;
    std::vector<std::uint8_t> heldSent;
    std::vector<std::vector<std::uint8_t>> armorSent;
    bool sentAny = false;
    const auto flush = [&](std::uint64_t t) {
        const auto at = std::max(t, spawnAt);
        const auto& slot = inventory[static_cast<std::size_t>(selected)];
        const auto& held = slot.empty() ? EmptyItem() : slot;
        if (!sentAny || held != heldSent) {
            stream_.push_back({at, Id::MobEquipment, GhostMobEquipment(held, selected)});
            heldSent = held;
        }
        if (!sentAny || armor != armorSent) {
            stream_.push_back({at, Id::MobArmorEquipment, GhostArmor(armor)});
            armorSent = armor;
        }
        sentAny = true;
    };
    for (std::size_t index = 0; index < changes.size(); ++index) {
        const auto& change = changes[index];
        if (change.selected) {
            // The server's PlayerHotBar only stands in for recordings made
            // before the client's own slot was sampled.
            if (change.sampled || !sampledSlots) selected = *change.selected;
        } else {
            auto& window = change.window == Equip::ArmorWindow ? armor : inventory;
            if (change.content) {
                for (std::size_t slot = 0; slot < window.size(); ++slot) {
                    window[slot] = slot < change.content->size() ? (*change.content)[slot] : std::vector<std::uint8_t>{};
                }
            } else if (change.slot < window.size()) {
                window[change.slot] = change.item;
            }
        }
        // Everything before the ghost exists collapses into its first equipment.
        const bool sameMomentNext = index + 1 < changes.size()
            && std::max(changes[index + 1].t, spawnAt) == std::max(change.t, spawnAt);
        if (!sameMomentNext) flush(change.t);
    }
}

ClientPacketResult Session::OnClientPacket(std::span<const std::uint8_t> packet, Clock::time_point now,
    std::vector<std::vector<std::uint8_t>>& out) {
    const auto header = ReplayCapture::ParseHeader(packet);
    if (!header) return {};
    const auto body = Body(packet, *header);
    switch (header->id) {
    case Id::RequestNetworkSettings: {
        if (stage_ != Stage::WaitingForSettings) return {};
        RakNet::Reader reader(body);
        const auto protocol = static_cast<std::int32_t>(reader.U32BE());
        if (!reader.Ok()) return {false, "Minecraft sent an invalid network-settings request."};
        if (protocol != SupportedBuild::NetworkProtocol) {
            out.push_back(PlayStatusPacket(protocol < SupportedBuild::NetworkProtocol
                ? LoginFailedClient : LoginFailedServer));
            return {false, std::format("Minecraft protocol {} does not match replay protocol {}.",
                protocol, SupportedBuild::NetworkProtocol)};
        }
        out.push_back(NetworkSettingsPacket());
        compression_ = true;
        stage_ = Stage::WaitingForLogin;
        return {};
    }
    case Id::Login:
        if (stage_ != Stage::WaitingForLogin) return {};
        // The login identity is never read or kept: it only moves us on.
        out.push_back(PlayStatusPacket(0));
        out.push_back(packsInfo_);
        stage_ = Stage::WaitingForPacks;
        return {};
    case Id::ResourcePackClientResponse: {
        RakNet::Reader reader(body);
        const auto status = reader.VarU32();
        if (!reader.Ok()) return {false, "Minecraft sent an invalid resource-pack response."};
        if (status == 0) {
            return {false, "Minecraft declined the replay's resource packs."};
        } else if (status == PacksSendPacks) {
            if (packDataInfo_.empty() || packChunks_.empty()) {
                return {false, "The replay requires resource-pack data that was not recorded."};
            }
            out.insert(out.end(), packDataInfo_.begin(), packDataInfo_.end());
            stage_ = Stage::WaitingForPacks;
        } else if (status == PacksHaveAll && !stackSent_) {
            out.push_back(packStack_);
            stackSent_ = true;
            stage_ = Stage::WaitingForPacksCompleted;
        } else if (status == PacksCompleted && stage_ < Stage::CatchingUp) {
            if (!stackSent_) {
                out.push_back(packStack_);
                stackSent_ = true;
            }
            stage_ = Stage::CatchingUp;
            anchorMicros_ = static_cast<double>(clipStart_);
            anchorAt_ = now;
        } else if (status > PacksCompleted) {
            return {false, std::format("Minecraft sent unsupported resource-pack status {}.", status)};
        }
        return {};
    }
    case Id::ResourcePackChunkRequest: {
        const auto requested = PackChunkKey(body);
        if (!requested) return {false, "Minecraft sent an invalid resource-pack chunk request."};
        const auto found = std::ranges::find_if(packChunks_, [&](const PackChunk& chunk) {
            return chunk.uuid == requested->first && chunk.index == requested->second;
        });
        if (found == packChunks_.end()) {
            return {false, std::format("The replay is missing resource-pack chunk {} for {}.",
                requested->second, requested->first)};
        }
        out.push_back(found->bytes);
        return {};
    }
    default:
        return {};
    }
}

double Session::AbsoluteMicros(Clock::time_point now) const noexcept {
    if (stage_ != Stage::Playing && stage_ != Stage::Finished) return static_cast<double>(clipStart_);
    if (paused_) return anchorMicros_;
    const auto elapsed = std::chrono::duration<double, std::micro>(now - anchorAt_).count();
    return std::min(anchorMicros_ + elapsed * static_cast<double>(speed_), static_cast<double>(endMicros_));
}

std::uint64_t Session::PositionMicros(Clock::time_point now) const noexcept {
    const auto absolute = AbsoluteMicros(now);
    return absolute > static_cast<double>(clipStart_) ? static_cast<std::uint64_t>(absolute) - clipStart_ : 0;
}

// Packets a rewind re-sends from before the player spawned: the world and
// what is in it. Everything else there (registries, settings, the join
// itself) is one-time and must not reach the client twice.
bool ResentBeforeSpawn(std::uint32_t id) noexcept {
    switch (id) {
    case 10:   // SetTime
    case 12:   // AddPlayer
    case 13:   // AddActor
    case 15:   // AddItemActor
    case 21:   // UpdateBlock
    case 31:   // MobEquipment
    case 32:   // MobArmorEquipment
    case 39:   // SetActorData
    case 56:   // BlockActorData
    case 58:   // LevelChunk
    case 172:  // UpdateSubChunkBlocks
    case 174:  // SubChunk
        return true;
    default:
        return false;
    }
}

bool Session::Admit(std::size_t index, const Packet& packet) {
    if (packet.ghostSpawn) {
        if (ghostSpawned_) return false;
        ghostSpawned_ = true;
        return true;
    }
    if (rebuilding_) {
        if (packet.id == ReplayCapture::PacketId::PlayStatus) return false;
        if (index < spawnIndex_ && !ResentBeforeSpawn(packet.id)) return false;
    }
    // Track what the replay spawned, so a rewind can remove it again.
    constexpr std::uint32_t AddActor = 13, RemoveActor = 14, AddItemActor = 15, AddPainting = 22;
    if (packet.id == AddActor || packet.id == AddItemActor || packet.id == AddPainting || packet.id == RemoveActor) {
        const auto header = ReplayCapture::ParseHeader(packet.bytes);
        RakNet::Reader reader(std::span(packet.bytes).subspan(header ? header->length : 0));
        const auto unique = ZigZagDecode(reader.VarU64());
        if (header && reader.Ok()) {
            if (packet.id == RemoveActor) std::erase(spawned_, unique);
            else if (std::ranges::find(spawned_, unique) == spawned_.end()) spawned_.push_back(unique);
        }
    }
    return true;
}

bool Session::SeekTo(std::uint64_t positionMicros, Clock::time_point now) {
    if (stage_ < Stage::CatchingUp) return false;
    const auto target = std::min(clipStart_ + positionMicros, endMicros_);
    const double before = AbsoluteMicros(now);
    const bool rewind = static_cast<double>(target) + 1000.0 < before && afterStartGame_ != 0;
    if (rewind) {
        // Blocks placed since the world's start go back to air; the replay
        // then places them again as it reaches them. (A block the recording
        // broke is not restored: what stood there is not in the stream.)
        if (blockIdsHashed_) {
            constexpr std::uint32_t UpdateBlock = 21;
            constexpr std::uint32_t NeighboursAndNetwork = 3;
            for (const auto& block : blockTouches_) {
                if (static_cast<double>(block.firstT) > before || block.firstRuntimeId == AirBlockHash) continue;
                RakNet::Writer writer;
                writer.VarU32(UpdateBlock);
                writer.VarU32(block.x);
                writer.VarU32(block.y);
                writer.VarU32(block.z);
                writer.VarU32(AirBlockHash);
                writer.VarU32(NeighboursAndNetwork);
                writer.VarU32(block.layer);
                prefix_.push_back(writer.Take());
            }
        }
        constexpr std::uint32_t RemoveActor = 14;
        for (const auto unique : spawned_) {
            RakNet::Writer writer;
            writer.VarU32(RemoveActor);
            writer.Bytes(ZigZagVarint64(unique));
            prefix_.push_back(writer.Take());
        }
        spawned_.clear();
        next_ = afterStartGame_;
        rebuilding_ = true;
    }
    stage_ = Stage::Playing;
    anchorMicros_ = static_cast<double>(target);
    anchorAt_ = now;
    return rewind;
}

void Session::Pump(Clock::time_point now, std::size_t budgetBytes, std::vector<std::vector<std::uint8_t>>& out) {
    if (stage_ != Stage::CatchingUp && stage_ != Stage::Playing) return;
    for (auto& packet : prefix_) out.push_back(std::move(packet));
    prefix_.clear();
    std::size_t bytes = 0;
    while (next_ < stream_.size() && (out.empty() || bytes < budgetBytes)) {
        const auto& packet = stream_[next_];
        if (stage_ == Stage::CatchingUp && packet.t >= clipStart_) {
            // The world before the clip has been rebuilt; play in real time.
            stage_ = Stage::Playing;
            anchorMicros_ = static_cast<double>(clipStart_);
            anchorAt_ = now;
        }
        if (stage_ == Stage::Playing && static_cast<double>(packet.t) > AbsoluteMicros(now)) break;
        if (Admit(next_, packet)) {
            bytes += packet.bytes.size();
            out.push_back(packet.bytes);
        }
        ++next_;
    }
    if (next_ >= stream_.size()) {
        if (stage_ == Stage::CatchingUp) {
            stage_ = Stage::Playing;
            anchorMicros_ = static_cast<double>(clipStart_);
            anchorAt_ = now;
        }
        if (stage_ == Stage::Playing && AbsoluteMicros(now) >= static_cast<double>(endMicros_)) stage_ = Stage::Finished;
    }
}

void Session::BeginInjected(Clock::time_point now) {
    if (stage_ >= Stage::CatchingUp) return;
    compression_ = true;
    stackSent_ = true;
    stage_ = Stage::CatchingUp;
    anchorMicros_ = static_cast<double>(clipStart_);
    anchorAt_ = now;
}

void Session::SetPaused(bool paused, Clock::time_point now) {
    if (paused == paused_) return;
    anchorMicros_ = AbsoluteMicros(now);
    anchorAt_ = now;
    paused_ = paused;
}

void Session::SetSpeed(float speed, Clock::time_point now) {
    anchorMicros_ = AbsoluteMicros(now);
    anchorAt_ = now;
    speed_ = std::clamp(speed, 0.25F, 4.0F);
}

}
