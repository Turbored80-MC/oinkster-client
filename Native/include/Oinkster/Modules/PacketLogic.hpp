#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Pure decoders for the few server->client game packets modules read (the
// player list, chat and titles, forms). Unit-tested against layouts taken
// from packets recorded on Minecraft 1.26.5203; nothing here touches the
// game. Every decoder is bounds-checked and returns nothing on any mismatch.
namespace Oinkster::Modules {

namespace GamePacket {
inline constexpr std::uint32_t Text = 9;
inline constexpr std::uint32_t StartGame = 11;
inline constexpr std::uint32_t AddPlayer = 12;
inline constexpr std::uint32_t MoveActorAbsolute = 18;
inline constexpr std::uint32_t MovePlayer = 19;
inline constexpr std::uint32_t ChangeDimension = 61;
inline constexpr std::uint32_t PlayerSkin = 93;
inline constexpr std::uint32_t MoveActorDelta = 111;
inline constexpr std::uint32_t PlayerList = 63;
inline constexpr std::uint32_t SetTitle = 88;
inline constexpr std::uint32_t ModalFormRequest = 100;
}

// The packet id and body of a whole packet (header varint first).
struct PacketView {
    std::uint32_t id{};
    std::span<const std::uint8_t> body;
};
[[nodiscard]] std::optional<PacketView> SplitPacket(std::span<const std::uint8_t> packet) noexcept;

inline constexpr std::uint32_t MaxListedSkinEdge = 128;

struct ListedPlayer {
    std::array<std::uint8_t, 16> uuid{};
    std::int64_t uniqueId{};
    std::string name;   // as sent (may hold colour codes)
    std::string xuid;   // decimal, empty or "0" for server NPCs
    std::int32_t platform{};
    std::uint32_t skinWidth{};
    std::uint32_t skinHeight{};
    std::vector<std::uint8_t> skin;  // RGBA8, skinWidth * skinHeight * 4, or empty
    std::uint32_t capeWidth{};
    std::uint32_t capeHeight{};
    std::vector<std::uint8_t> cape;  // RGBA8 or empty
    // The whole SerializedSkin exactly as sent, when its every field decoded
    // (ID ... ProfileHash); what a PlayerSkin packet carries.
    std::vector<std::uint8_t> serializedSkin;
};

struct PlayerListUpdate {
    bool remove{};
    std::vector<ListedPlayer> players;  // remove: only uuid is set
};

// PlayerList (63). Each entry starts with a two-byte add/remove marker; an
// add carries uuid, unique id, name, XUID, platform chat id, build platform,
// the SerializedSkin (fields in its cereal order: ID, PlayFabID,
// ResourcePatch, ImageData, AnimatedImageData, CapeImageData, GeometryData,
// GeometryDataMinEngineVersion, AnimationData, CapeID, FullID, ArmSize u8,
// SkinColor u32, PersonaPieces, PieceTintColors, five flags,
// TrustedSkinFlag and ProfileHash as strings), then teacher / host /
// sub-client flags and the player colour. The next entry must start where
// that decode ends; if it does not (a field this build added), it is found
// by its own well-formed header and that skin is not kept raw.
[[nodiscard]] std::optional<PlayerListUpdate> ParsePlayerList(std::span<const std::uint8_t> body);

// The 8x8 front of the head (base with the hat layer over it), ARGB, row
// major; nothing for an image that is not a classic 64x64/128x128 atlas.
[[nodiscard]] std::optional<std::array<std::uint32_t, 64>> HeadFace(std::uint32_t width, std::uint32_t height,
    std::span<const std::uint8_t> rgba) noexcept;

// StartGame (11): the local player's actor unique id (its first field),
// which identifies the local player's own PlayerList entry.
[[nodiscard]] std::optional<std::int64_t> ParseStartGameUniqueId(std::span<const std::uint8_t> body);

// A PlayerSkin (93) packet giving `uuid` the serialized skin `skin`.
[[nodiscard]] std::vector<std::uint8_t> BuildPlayerSkinPacket(const std::array<std::uint8_t, 16>& uuid,
    std::span<const std::uint8_t> serializedSkin, std::string_view newName, std::string_view oldName);
[[nodiscard]] std::wstring SanitizeSkinFileName(std::string_view name);

// A PNG file (RGBA8, stored deflate blocks) for saving skins and capes.
[[nodiscard]] std::vector<std::uint8_t> EncodePng(std::uint32_t width, std::uint32_t height, std::span<const std::uint8_t> rgba);

// The server's player list as it stands: upserts on add, erases on remove.
class PlayerRoster {
public:
    static constexpr std::size_t MaxPlayers = 512;
    void Apply(PlayerListUpdate update);
    void Clear() noexcept { players_.clear(); }
    [[nodiscard]] const std::vector<ListedPlayer>& Players() const noexcept { return players_; }
    [[nodiscard]] const ListedPlayer* Find(std::string_view cleanName) const;
    [[nodiscard]] const ListedPlayer* FindByUniqueId(std::int64_t uniqueId) const;

private:
    std::vector<ListedPlayer> players_;
};

// Text (9): the message type, the sender (chat/whisper/announcement) and
// the message, with any localisation parameters.
struct TextMessage {
    std::uint8_t type{};
    std::string source;
    std::string message;
    std::vector<std::string> parameters;
};
[[nodiscard]] std::optional<TextMessage> ParseText(std::span<const std::uint8_t> body);

// SetTitle (88): type (2 = title, 3 = subtitle, 4 = action bar ...) and text.
struct TitleMessage {
    std::int32_t type{};
    std::string text;
};
[[nodiscard]] std::optional<TitleMessage> ParseTitle(std::span<const std::uint8_t> body);

// ModalFormRequest (100): the form id and its JSON.
struct FormRequest {
    std::uint32_t id{};
    std::string json;
};
[[nodiscard]] std::optional<FormRequest> ParseFormRequest(std::span<const std::uint8_t> body);

}
