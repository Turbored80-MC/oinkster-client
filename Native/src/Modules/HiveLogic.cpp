#include "Oinkster/Modules/HiveLogic.hpp"
#include "Oinkster/Json.hpp"
#include <algorithm>
#include <cctype>

namespace Oinkster::Modules {
namespace {

std::string Lower(std::string_view text) {
    std::string value(text);
    std::ranges::transform(value, value.begin(), [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
    return value;
}

std::string Trim(std::string_view text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return {};
    const auto last = text.find_last_not_of(" \t\r\n");
    return std::string(text.substr(first, last - first + 1));
}

bool StartsWith(std::string_view text, std::string_view prefix) noexcept { return text.substr(0, prefix.size()) == prefix; }

// A player name out of an invite line: drops leading arrows/symbols and
// trailing punctuation, keeps letters, digits, spaces and underscores.
std::string CleanName(std::string_view text) {
    std::string name = Trim(text);
    while (!name.empty() && !std::isalnum(static_cast<unsigned char>(name.front())) && name.front() != '_') name.erase(name.begin());
    while (!name.empty() && !std::isalnum(static_cast<unsigned char>(name.back())) && name.back() != '_') name.pop_back();
    return name;
}
}

std::string StripFormatting(std::string_view text) {
    std::string clean;
    clean.reserve(text.size());
    for (std::size_t at = 0; at < text.size();) {
        // U+00A7 is C2 A7 in UTF-8; a lone A7 byte is tolerated too.
        if (at + 1 < text.size() && static_cast<unsigned char>(text[at]) == 0xC2 && static_cast<unsigned char>(text[at + 1]) == 0xA7) {
            at += 3;
            continue;
        }
        if (static_cast<unsigned char>(text[at]) == 0xA7) {
            at += 2;
            continue;
        }
        clean.push_back(text[at++]);
    }
    return clean;
}

HiveEvent ClassifyHiveChat(std::string_view message) {
    const std::string clean = Trim(StripFormatting(message));
    // Hive prefixes system lines with "» ".
    std::string body = clean;
    if (StartsWith(body, "\xC2\xBB ")) body = Trim(std::string_view(body).substr(3));
    if (body == "Game OVER!") return {HiveEventKind::GameOver, {}};
    if (StartsWith(body, "You finished all maps and came in") || StartsWith(body, "You finished in")) {
        return {HiveEventKind::Finished, {}};
    }
    if (const auto at = body.find("You are on the "); at != std::string::npos) {
        std::string team = Trim(std::string_view(body).substr(at + 15));
        while (!team.empty() && (team.back() == '!' || team.back() == '.')) team.pop_back();
        if (!team.empty()) return {HiveEventKind::TeamAssigned, team};
    }
    if (const auto at = body.find(" has been ELIMINATED!"); at != std::string::npos) {
        return {HiveEventKind::TeamEliminated, Trim(std::string_view(body).substr(0, at))};
    }
    if (const auto at = clean.find(" wants you to join their party!"); at != std::string::npos) {
        const auto name = CleanName(std::string_view(clean).substr(0, at));
        if (!name.empty()) return {HiveEventKind::PartyInvite, name};
    }
    constexpr std::string_view FriendPrefix = "You received a friend invite from ";
    if (const auto at = clean.find(FriendPrefix); at != std::string::npos) {
        const auto name = CleanName(std::string_view(clean).substr(at + FriendPrefix.size()));
        if (!name.empty()) return {HiveEventKind::FriendInvite, name};
    }
    constexpr std::string_view ConnectedPrefix = "You are connected to server name ";
    if (const auto at = clean.find(ConnectedPrefix); at != std::string::npos) {
        const auto game = HiveGameCode(Trim(std::string_view(clean).substr(at + ConnectedPrefix.size())));
        if (!game.empty()) return {HiveEventKind::ConnectedServer, game};
    }
    return {};
}

HiveEvent ClassifyHiveTitle(std::string_view title) {
    const std::string clean = Trim(StripFormatting(title));
    if (clean == "You died!" || clean == "You're spectating the show!") return {HiveEventKind::Died, {}};
    constexpr std::string_view JoinPrefix = "Join Code:";
    if (StartsWith(clean, JoinPrefix)) {
        const auto code = Trim(std::string_view(clean).substr(JoinPrefix.size()));
        if (!code.empty()) return {HiveEventKind::JoinCode, code};
    }
    return {};
}

std::string HiveGameCode(std::string_view serverName) {
    std::string code;
    for (const char c : Trim(serverName)) {
        if (std::isdigit(static_cast<unsigned char>(c))) continue;
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_') code.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }
    while (!code.empty() && (code.back() == '-' || code.back() == '_')) code.pop_back();
    return code.size() > 32 ? std::string{} : code;
}

bool IsHiveHub(std::string_view gameCode) noexcept {
    return gameCode == "HUB" || gameCode.starts_with("HUB-") || gameCode == "REPLAY";
}

std::vector<std::string> ParseMapList(std::string_view text) {
    std::vector<std::string> names;
    std::size_t start = 0;
    for (std::size_t at = 0; at <= text.size(); ++at) {
        if (at == text.size() || text[at] == ',') {
            auto name = Lower(Trim(text.substr(start, at - start)));
            if (!name.empty() && names.size() < 32) names.push_back(std::move(name));
            start = at + 1;
        }
    }
    return names;
}

std::optional<std::vector<std::string>> MapVoteButtons(std::string_view json) {
    const auto document = Json::Parse(json);
    if (!document) return std::nullopt;
    const auto* title = document->Find("title");
    if (!title || !title->AsString() || Lower(Trim(StripFormatting(*title->AsString()))) != "choose map") return std::nullopt;
    const auto* buttons = document->Find("buttons");
    if (!buttons || !buttons->AsArray()) return std::nullopt;
    std::vector<std::string> names;
    for (const auto& button : *buttons->AsArray()) {
        const auto* text = button.Find("text");
        std::string name = text && text->AsString() ? StripFormatting(*text->AsString()) : std::string{};
        if (const auto newline = name.find('\n'); newline != std::string::npos) name.resize(newline);
        names.push_back(Trim(name));
    }
    return names;
}

std::optional<std::size_t> ChooseMapButton(std::span<const std::string> buttons, std::span<const std::string> preferred,
    std::span<const std::string> avoided) {
    const auto matches = [](const std::string& button, const std::string& wanted) {
        const auto lower = Lower(button);
        return !wanted.empty() && lower.find(wanted) != std::string::npos;
    };
    for (const auto& wanted : preferred) {
        for (std::size_t index = 0; index < buttons.size(); ++index) {
            if (matches(buttons[index], wanted)) return index;
        }
    }
    if (avoided.empty()) return std::nullopt;
    for (std::size_t index = 0; index < buttons.size(); ++index) {
        const bool avoid = std::ranges::any_of(avoided, [&](const std::string& name) { return matches(buttons[index], name); });
        // "Random" is a button too; never pick it on its own.
        if (!avoid && Lower(buttons[index]).find("random") == std::string::npos && !buttons[index].empty()) return index;
    }
    return std::nullopt;
}

}
