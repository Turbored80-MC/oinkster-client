#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Pure helpers for Hive Utils: recognising The Hive's chat and title
// messages and choosing map-vote buttons. The message texts are The Hive's
// own (the same ones Flarial's HiveUtils keys on), compared after colour
// codes are removed so a palette change does not break them.
namespace Oinkster::Modules {

// Removes Minecraft formatting codes (section sign + one character).
[[nodiscard]] std::string StripFormatting(std::string_view text);

enum class HiveEventKind : std::uint8_t {
    None,
    GameOver,         // the round ended for this player
    Finished,         // solo games: finished all maps / finished in place N
    TeamAssigned,     // value = the team name
    TeamEliminated,   // value = the eliminated team
    PartyInvite,      // value = who invited
    FriendInvite,     // value = who invited
    ConnectedServer,  // value = the game code (digits removed), e.g. "SKY-DUOS"
    Died,             // title: died / now spectating
    JoinCode,         // title: value = the custom-server join code
};

struct HiveEvent {
    HiveEventKind kind{HiveEventKind::None};
    std::string value;
};

[[nodiscard]] HiveEvent ClassifyHiveChat(std::string_view message);
[[nodiscard]] HiveEvent ClassifyHiveTitle(std::string_view title);

// "SKY-DUOS12" -> "SKY-DUOS"; "HUB-BED3" -> "HUB-BED".
[[nodiscard]] std::string HiveGameCode(std::string_view serverName);
[[nodiscard]] bool IsHiveHub(std::string_view gameCode) noexcept;

// Splits a comma-separated, case-insensitive list ("Fort, Dragon Isle").
[[nodiscard]] std::vector<std::string> ParseMapList(std::string_view text);

// The "Choose Map" form's buttons, first line of each with formatting removed;
// nothing when `json` is not that form.
[[nodiscard]] std::optional<std::vector<std::string>> MapVoteButtons(std::string_view json);
// The first preferred map present, else the first button not avoided (only
// when something is avoided), else nothing.
[[nodiscard]] std::optional<std::size_t> ChooseMapButton(std::span<const std::string> buttons,
    std::span<const std::string> preferred, std::span<const std::string> avoided);

}
