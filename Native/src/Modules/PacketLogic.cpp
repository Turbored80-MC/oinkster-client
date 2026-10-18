#include "Oinkster/Modules/PacketLogic.hpp"
#include "Oinkster/Modules/HudFormat.hpp"
#include "Oinkster/RakNet.hpp"
#include "Oinkster/ReplayInjector.hpp"
#include "Oinkster/SkinHead.hpp"
#include <algorithm>
#include <cctype>
#include <cwctype>

namespace Oinkster::Modules {
namespace {
using RakNet::Reader;

constexpr std::size_t MaxString = 1U << 20;
// Server NPCs use two-line name tags with colour codes and JSON.
constexpr std::size_t MaxNameBytes = 256;
std::optional<std::string> ReadString(Reader& reader, std::size_t limit = MaxString) {
    const auto length = reader.VarU32();
    if (!reader.Ok() || length > limit || length > reader.Remaining()) return std::nullopt;
    const auto bytes = reader.Bytes(length);
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

bool SkipString(Reader& reader) {
    const auto length = reader.VarU32();
    if (!reader.Ok() || length > reader.Remaining()) return false;
    reader.Skip(length);
    return reader.Ok();
}

std::uint32_t U32LE(Reader& reader) {
    const auto bytes = reader.Bytes(4);
    if (bytes.size() != 4) return 0;
    return static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8U)
        | (static_cast<std::uint32_t>(bytes[2]) << 16U) | (static_cast<std::uint32_t>(bytes[3]) << 24U);
}

std::int64_t ZigZag(std::uint64_t value) noexcept {
    return static_cast<std::int64_t>(value >> 1U) ^ -static_cast<std::int64_t>(value & 1U);
}

bool PrintableName(std::string_view name) noexcept {
    if (name.empty() || name.size() > MaxNameBytes) return false;
    return std::ranges::none_of(name, [](char c) {
        return (static_cast<unsigned char>(c) < 0x20 && c != '\n' && c != '\r') || c == 0x7F;
    });
}

bool DecimalOrEmpty(std::string_view value) noexcept {
    return value.size() <= 20 && std::ranges::all_of(value, [](char c) { return c >= '0' && c <= '9'; });
}

// Whether an add entry's header (marker, uuid, unique id, name, XUID,
// platform chat id, build platform) starts at `at`.
bool EntryHeaderAt(std::span<const std::uint8_t> body, std::size_t at) {
    if (at + 2 + 16 + 4 > body.size() || body[at] != 1 || body[at + 1] != 0) return false;
    Reader reader(body.subspan(at + 2 + 16));
    (void)reader.VarU64();
    const auto name = ReadString(reader, MaxNameBytes);
    const auto xuid = ReadString(reader, 20);
    const auto chat = ReadString(reader, 64);
    const auto platform = static_cast<std::int32_t>(U32LE(reader));
    return reader.Ok() && name && PrintableName(*name) && xuid && DecimalOrEmpty(*xuid) && chat
        && platform >= -1 && platform <= 64;
}

// Skips a SerializedSkin's fields after FullID and the entry's own trailing
// fields (see ParsePlayerList). False on anything out of shape.
bool SkipSkinTail(Reader& reader, std::size_t& skinEnd) {
    (void)reader.U8();        // ArmSize
    (void)U32LE(reader);      // SkinColor
    const auto pieces = reader.VarU32();
    if (!reader.Ok() || pieces > 256) return false;
    for (std::uint32_t index = 0; index < pieces; ++index) {
        if (!SkipString(reader) || !SkipString(reader) || !SkipString(reader)) return false;
        (void)reader.U8();
        if (!SkipString(reader)) return false;
    }
    const auto tints = reader.VarU32();
    if (!reader.Ok() || tints > 256) return false;
    for (std::uint32_t index = 0; index < tints; ++index) {
        if (!SkipString(reader)) return false;
        const auto colours = reader.VarU32();
        if (!reader.Ok() || colours > 64) return false;
        for (std::uint32_t colour = 0; colour < colours; ++colour) {
            if (!SkipString(reader)) return false;
        }
    }
    for (int flag = 0; flag < 5; ++flag) {
        if (reader.U8() > 1) return false;
    }
    const auto trusted = ReadString(reader, 16);
    if (!trusted || !SkipString(reader)) return false;   // TrustedSkinFlag, ProfileHash
    skinEnd = reader.Position();
    for (int flag = 0; flag < 3; ++flag) {
        if (reader.U8() > 1) return false;               // teacher, host, sub-client
    }
    (void)U32LE(reader);                                 // player colour
    return reader.Ok();
}

// Reads one add entry from its uuid through the skin's FullId.
bool ReadAddEntry(Reader& reader, ListedPlayer& player, std::size_t& skinStart) {
    const auto uuid = reader.Bytes(16);
    if (uuid.size() != 16) return false;
    std::copy(uuid.begin(), uuid.end(), player.uuid.begin());
    player.uniqueId = ZigZag(reader.VarU64());
    auto name = ReadString(reader, MaxNameBytes);
    auto xuid = ReadString(reader, 20);
    if (!name || !xuid || !SkipString(reader)) return false;
    player.name = std::move(*name);
    player.xuid = std::move(*xuid);
    player.platform = static_cast<std::int32_t>(U32LE(reader));
    skinStart = reader.Position();
    // SerializedSkin: id, PlayFab id, resource patch, then the skin image.
    if (!SkipString(reader) || !SkipString(reader) || !SkipString(reader)) return false;
    const auto width = U32LE(reader);
    const auto height = U32LE(reader);
    const auto imageBytes = reader.VarU32();
    if (!reader.Ok() || imageBytes > reader.Remaining()) return false;
    const auto image = reader.Bytes(imageBytes);
    if (width <= MaxListedSkinEdge && height <= MaxListedSkinEdge && image.size() == static_cast<std::size_t>(width) * height * 4) {
        player.skinWidth = width;
        player.skinHeight = height;
        player.skin.assign(image.begin(), image.end());
    }
    // Animations: a count, then image + type + frames + expression each.
    const auto animations = reader.VarU32();
    if (!reader.Ok() || animations > 64) return false;
    for (std::uint32_t index = 0; index < animations; ++index) {
        (void)U32LE(reader);
        (void)U32LE(reader);
        if (!SkipString(reader)) return false;
        reader.Skip(12);
    }
    // Cape image, geometry, geometry engine version, animation data, cape id, full id.
    const auto capeWidth = U32LE(reader);
    const auto capeHeight = U32LE(reader);
    const auto capeBytes = reader.VarU32();
    if (!reader.Ok() || capeBytes > reader.Remaining()) return false;
    const auto cape = reader.Bytes(capeBytes);
    if (capeWidth <= MaxListedSkinEdge && capeHeight <= MaxListedSkinEdge && !cape.empty()
        && cape.size() == static_cast<std::size_t>(capeWidth) * capeHeight * 4) {
        player.capeWidth = capeWidth;
        player.capeHeight = capeHeight;
        player.cape.assign(cape.begin(), cape.end());
    }
    for (int field = 0; field < 5; ++field) {
        if (!SkipString(reader)) return false;
    }
    return reader.Ok();
}
}

std::optional<PacketView> SplitPacket(std::span<const std::uint8_t> packet) noexcept {
    Reader reader(packet);
    const auto header = reader.VarU32();
    if (!reader.Ok()) return std::nullopt;
    return PacketView{header & 0x3FFU, packet.subspan(reader.Position())};
}

std::optional<PlayerListUpdate> ParsePlayerList(std::span<const std::uint8_t> body) {
    Reader reader(body);
    const auto count = reader.VarU32();
    if (!reader.Ok() || count == 0 || count > PlayerRoster::MaxPlayers || reader.Remaining() < 2) return std::nullopt;
    PlayerListUpdate update;
    const auto first = reader.Rest();
    update.remove = first[0] == 0 && first[1] == 1;
    if (!update.remove && !(first[0] == 1 && first[1] == 0)) return std::nullopt;
    if (update.remove) {
        for (std::uint32_t index = 0; index < count; ++index) {
            const auto marker = reader.Bytes(2);
            const auto uuid = reader.Bytes(16);
            if (marker.size() != 2 || marker[0] != 0 || marker[1] != 1 || uuid.size() != 16) return std::nullopt;
            ListedPlayer player;
            std::copy(uuid.begin(), uuid.end(), player.uuid.begin());
            update.players.push_back(std::move(player));
        }
        return update;
    }
    std::size_t at = reader.Position();
    for (std::uint32_t index = 0; index < count; ++index) {
        if (!EntryHeaderAt(body, at)) return std::nullopt;
        const std::size_t entryStart = at + 2;
        Reader entry(body.subspan(entryStart));
        ListedPlayer player;
        std::size_t skinStart{};
        if (!ReadAddEntry(entry, player, skinStart)) return std::nullopt;
        // Exact decode: the entry ends where the next one (or the packet) starts.
        std::size_t skinEnd{};
        const bool exact = SkipSkinTail(entry, skinEnd)
            && (index + 1 == count ? entryStart + entry.Position() == body.size() : EntryHeaderAt(body, entryStart + entry.Position()));
        if (!exact) return std::nullopt;
        const auto skin = body.subspan(entryStart + skinStart, skinEnd - skinStart);
        player.serializedSkin.assign(skin.begin(), skin.end());
        update.players.push_back(std::move(player));
        if (index + 1 == count) break;
        at = entryStart + entry.Position();
    }
    return update;
}

std::optional<std::array<std::uint32_t, 64>> HeadFace(std::uint32_t width, std::uint32_t height,
    std::span<const std::uint8_t> rgba) noexcept {
    SkinHeadTexture texture;
    if (!ExtractSkinHead(width, height, rgba, texture)) return std::nullopt;
    const auto front = static_cast<std::size_t>(SkinFace::Front);
    const std::uint32_t edge = texture.faceEdge;
    if (edge == 0 || edge > SkinHeadTexture::MaxFaceEdge) return std::nullopt;
    const std::uint32_t step = edge / 8;
    std::array<std::uint32_t, 64> face{};
    for (std::uint32_t y = 0; y < 8; ++y) {
        for (std::uint32_t x = 0; x < 8; ++x) {
            const std::size_t source = static_cast<std::size_t>(y * step) * SkinHeadTexture::MaxFaceEdge + x * step;
            // SkinHead packs pixels R in the low byte (ABGR); faces here are ARGB.
            const auto argb = [](std::uint32_t abgr) {
                return (abgr & 0xFF00FF00U) | ((abgr & 0xFFU) << 16U) | ((abgr >> 16U) & 0xFFU);
            };
            const std::uint32_t base = argb(texture.base[front][source]) | 0xFF000000U;
            const std::uint32_t hat = argb(texture.overlay[front][source]);
            face[static_cast<std::size_t>(y) * 8 + x] = (hat >> 24U) >= 128 ? (hat | 0xFF000000U) : base;
        }
    }
    return face;
}

void PlayerRoster::Apply(PlayerListUpdate update) {
    for (auto& player : update.players) {
        const auto found = std::ranges::find(players_, player.uuid, &ListedPlayer::uuid);
        if (update.remove) {
            if (found != players_.end()) players_.erase(found);
            continue;
        }
        if (found != players_.end()) *found = std::move(player);
        else if (players_.size() < MaxPlayers) players_.push_back(std::move(player));
    }
}

const ListedPlayer* PlayerRoster::Find(std::string_view cleanName) const {
    for (const auto& player : players_) {
        if (ReplayInjector::CleanPlayerName(player.name) == cleanName) return &player;
    }
    for (const auto& player : players_) {
        auto name = ReplayInjector::CleanPlayerName(player.name);
        std::ranges::transform(name, name.begin(), [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
        std::string wanted(cleanName);
        std::ranges::transform(wanted, wanted.begin(), [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
        if (name == wanted) return &player;
    }
    return nullptr;
}

const ListedPlayer* PlayerRoster::FindByUniqueId(std::int64_t uniqueId) const {
    const auto found = std::ranges::find(players_, uniqueId, &ListedPlayer::uniqueId);
    return found == players_.end() ? nullptr : &*found;
}

std::optional<std::int64_t> ParseStartGameUniqueId(std::span<const std::uint8_t> body) {
    Reader reader(body);
    const auto unique = ZigZag(reader.VarU64());
    if (!reader.Ok()) return std::nullopt;
    return unique;
}

std::vector<std::uint8_t> BuildPlayerSkinPacket(const std::array<std::uint8_t, 16>& uuid,
    std::span<const std::uint8_t> serializedSkin, std::string_view newName, std::string_view oldName) {
    RakNet::Writer writer;
    writer.VarU32(GamePacket::PlayerSkin);
    writer.Bytes(uuid);
    writer.Bytes(serializedSkin);
    for (const auto name : {newName, oldName}) {
        writer.VarU32(static_cast<std::uint32_t>(name.size()));
        writer.Bytes(std::span(reinterpret_cast<const std::uint8_t*>(name.data()), name.size()));
    }
    return writer.Take();
}

std::wstring SanitizeSkinFileName(std::string_view name) {
    std::wstring file;
    for (const wchar_t c : FromUtf8(name)) {
        file.push_back(c < 0x20 || std::wstring_view(L"<>:\"/\\|?*").find(c) != std::wstring_view::npos ? L'_' : c);
    }
    if (file.size() > 48) file.resize(48);
    while (!file.empty() && (file.back() == L'.' || file.back() == L' ')) file.pop_back();
    if (file.empty()) return L"player";
    std::wstring upper = file;
    std::ranges::transform(upper, upper.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towupper(c)); });
    const auto dot = upper.find(L'.');
    const std::wstring_view stem(upper.data(), dot == std::wstring::npos ? upper.size() : dot);
    constexpr std::array<std::wstring_view, 22> reserved{L"CON", L"PRN", L"AUX", L"NUL", L"COM1", L"COM2", L"COM3",
        L"COM4", L"COM5", L"COM6", L"COM7", L"COM8", L"COM9", L"LPT1", L"LPT2", L"LPT3", L"LPT4", L"LPT5",
        L"LPT6", L"LPT7", L"LPT8", L"LPT9"};
    if (std::ranges::find(reserved, stem) != reserved.end()) file.insert(file.begin(), L'_');
    return file;
}

std::vector<std::uint8_t> EncodePng(std::uint32_t width, std::uint32_t height, std::span<const std::uint8_t> rgba) {
    if (width == 0 || height == 0 || rgba.size() != static_cast<std::size_t>(width) * height * 4) return {};
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t n = 0; n < 256; ++n) {
        std::uint32_t c = n;
        for (int k = 0; k < 8; ++k) c = (c & 1U) ? 0xEDB88320U ^ (c >> 1U) : c >> 1U;
        table[n] = c;
    }
    const auto crc = [&](std::span<const std::uint8_t> bytes) {
        std::uint32_t c = 0xFFFFFFFFU;
        for (const auto b : bytes) c = table[(c ^ b) & 0xFFU] ^ (c >> 8U);
        return c ^ 0xFFFFFFFFU;
    };
    std::vector<std::uint8_t> out{0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    const auto be32 = [](std::vector<std::uint8_t>& bytes, std::uint32_t value) {
        for (int shift = 24; shift >= 0; shift -= 8) bytes.push_back(static_cast<std::uint8_t>(value >> shift));
    };
    const auto chunk = [&](const char* type, const std::vector<std::uint8_t>& data) {
        be32(out, static_cast<std::uint32_t>(data.size()));
        std::vector<std::uint8_t> body(type, type + 4);
        body.insert(body.end(), data.begin(), data.end());
        out.insert(out.end(), body.begin(), body.end());
        be32(out, crc(body));
    };
    std::vector<std::uint8_t> header;
    be32(header, width);
    be32(header, height);
    header.insert(header.end(), {8, 6, 0, 0, 0});   // 8-bit RGBA, no interlace
    chunk("IHDR", header);
    // Scanlines with filter 0, wrapped in zlib stored blocks.
    std::vector<std::uint8_t> raw;
    raw.reserve(static_cast<std::size_t>(height) * (width * 4 + 1));
    for (std::uint32_t y = 0; y < height; ++y) {
        raw.push_back(0);
        const auto row = rgba.subspan(static_cast<std::size_t>(y) * width * 4, static_cast<std::size_t>(width) * 4);
        raw.insert(raw.end(), row.begin(), row.end());
    }
    std::vector<std::uint8_t> zlib{0x78, 0x01};
    for (std::size_t at = 0; at < raw.size() || raw.empty();) {
        const std::size_t length = std::min<std::size_t>(65535, raw.size() - at);
        const bool last = at + length == raw.size();
        zlib.push_back(last ? 1 : 0);
        zlib.push_back(static_cast<std::uint8_t>(length));
        zlib.push_back(static_cast<std::uint8_t>(length >> 8U));
        zlib.push_back(static_cast<std::uint8_t>(~length));
        zlib.push_back(static_cast<std::uint8_t>(~length >> 8U));
        zlib.insert(zlib.end(), raw.begin() + static_cast<std::ptrdiff_t>(at), raw.begin() + static_cast<std::ptrdiff_t>(at + length));
        at += length;
        if (last) break;
    }
    std::uint32_t a = 1, b = 0;
    for (const auto byte : raw) {
        a = (a + byte) % 65521U;
        b = (b + a) % 65521U;
    }
    be32(zlib, (b << 16U) | a);
    chunk("IDAT", zlib);
    chunk("IEND", {});
    return out;
}

std::optional<TextMessage> ParseText(std::span<const std::uint8_t> body) {
    // needsTranslation, payload kind (0 message, 1 author + message,
    // 2 message + parameters), type, the payload, then XUID and platform id.
    Reader reader(body);
    (void)reader.U8();
    const auto kind = reader.U8();
    TextMessage text;
    text.type = reader.U8();
    if (!reader.Ok() || kind > 2) return std::nullopt;
    if (kind == 1) {
        auto source = ReadString(reader, 1024);
        if (!source) return std::nullopt;
        text.source = std::move(*source);
    }
    auto message = ReadString(reader, 64 * 1024);
    if (!message) return std::nullopt;
    text.message = std::move(*message);
    if (kind == 2) {
        const auto count = reader.VarU32();
        if (!reader.Ok() || count > 64) return std::nullopt;
        for (std::uint32_t index = 0; index < count; ++index) {
            auto parameter = ReadString(reader, 4096);
            if (!parameter) return std::nullopt;
            text.parameters.push_back(std::move(*parameter));
        }
    }
    return text;
}

std::optional<TitleMessage> ParseTitle(std::span<const std::uint8_t> body) {
    Reader reader(body);
    TitleMessage title;
    title.type = static_cast<std::int32_t>(ZigZag(reader.VarU32()));
    auto text = ReadString(reader, 4096);
    if (!reader.Ok() || !text) return std::nullopt;
    title.text = std::move(*text);
    return title;
}

std::optional<FormRequest> ParseFormRequest(std::span<const std::uint8_t> body) {
    Reader reader(body);
    FormRequest form;
    form.id = reader.VarU32();
    auto json = ReadString(reader, 256 * 1024);
    if (!reader.Ok() || !json) return std::nullopt;
    form.json = std::move(*json);
    return form;
}

}
