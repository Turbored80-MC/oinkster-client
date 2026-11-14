#pragma once

#include "Oinkster/Events.hpp"
#include "Oinkster/Json.hpp"
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The player's own global resource packs, read from disk rather than from the
// game: minecraftpe/global_resource_packs.json (the packs the player turned
// on under Settings > Global Resources, highest priority first) matched by
// header uuid against manifest.json in the player's resource_packs and
// development_resource_packs folders. World and server packs never appear in
// that file, so they never show up here; neither do the game's built-ins.
//
// Bedrock keeps one com.mojang folder per signed-in user plus a Shared one
// (%APPDATA%\Minecraft Bedrock\Users\<id|Shared>\games\com.mojang), and older
// installs keep the UWP LocalState one; the newest global_resource_packs.json
// across them is the live one. Imported packs are often nested one folder
// deep, so both levels are searched.
namespace Oinkster::UserResourcePacks {

struct Pack {
    std::string uuid;  // lowercased header uuid
    std::wstring name;  // display name, formatting codes removed
    std::wstring version;  // "1.10.1", empty when the manifest has none
    std::wstring author;  // who made it, empty when the pack does not say
    std::shared_ptr<const ArgbImage> icon;  // pack_icon.png, downscaled; null when unreadable
    std::filesystem::path folder;
    std::string subpack;
    bool active{};
    std::size_t priority{};  // zero is highest priority
};

struct StackEntry {
    std::string uuid;
    std::string subpack;
    std::string version;
    Json::Value source;  // complete original object, including unknown fields
};

struct Snapshot {
    std::filesystem::path path;
    std::filesystem::file_time_type written{};
    std::uintmax_t size{};
    std::string sourceJson;
    std::vector<Pack> packs;
    std::vector<StackEntry> active;
    std::wstring error;
    [[nodiscard]] bool Ready() const noexcept { return error.empty() && !path.empty(); }
};

enum class ApplyStatus : std::uint8_t {
    Applied,
    Conflict,
    Invalid,
    WriteFailed,
    ReloadUnavailable,
    ReloadFailed,
    RollbackFailed
};

struct ApplyResult {
    ApplyStatus status{ApplyStatus::Invalid};
    std::wstring message;
    [[nodiscard]] bool Succeeded() const noexcept { return status == ApplyStatus::Applied; }
};

using ReloadCallback = std::function<bool(std::span<const std::string> orderedUuids, std::wstring& error)>;

struct ManifestHeader {
    std::string uuid;
    std::string name;  // raw header.name (may be a lang key such as "pack.name")
    std::string version;
    std::string description;  // raw header.description (may be a lang key)
    std::vector<std::string> authors;  // metadata.authors
};

// Pure helpers (unit-tested).
// Pack ids of a global_resource_packs.json, lowercased, in file order.
[[nodiscard]] std::vector<std::string> ParseGlobalPackList(std::string_view json);
[[nodiscard]] std::optional<std::vector<StackEntry>> ParseGlobalPackEntries(std::string_view json);
[[nodiscard]] std::string SerializeGlobalPackEntries(std::span<const StackEntry> entries);
// header.uuid / name / version of a manifest.json (comments tolerated).
[[nodiscard]] std::optional<ManifestHeader> ParseManifestHeader(std::string_view manifest);
// The value of `key` in a .lang file ("key=value", "##" comments).
[[nodiscard]] std::optional<std::string> LookupLangValue(std::string_view lang, std::string_view key);
// header.name made presentable: lang keys resolved through `lang` when given,
// formatting codes and surrounding whitespace removed; `fallback` when empty.
[[nodiscard]] std::wstring DisplayName(std::string_view rawName, std::string_view lang, std::wstring_view fallback);

// Who made a pack: metadata.authors when present, else a "by <name>" /
// "made by <name>" credit in the description, else in the name. Formatting
// codes are removed and a leading '@' dropped; empty when nothing credible.
[[nodiscard]] std::wstring PackAuthor(const std::vector<std::string>& authors, std::string_view description, std::string_view name);

// Every pack installed in the player's folders, names only, no icons. Cheap
// enough to call once at module construction; never throws.
[[nodiscard]] std::vector<Pack> Installed() noexcept;

// Complete editable state for the newest Global Resources file. Snapshot and
// LoadSnapshot and ApplyStack are intentionally synchronous; only the reviewed
// reload callback crosses into game code. The Settings UI disables navigation
// while that call is in progress.
[[nodiscard]] Snapshot LoadSnapshot() noexcept;
[[nodiscard]] ApplyResult ApplyStack(const Snapshot& expected, std::span<const std::string> orderedActiveUuids,
    const ReloadCallback& reload) noexcept;

// Background watcher. Start/Stop are idempotent; Current is the latest
// active list (null until the first read). The file is re-checked every
// couple of seconds and the list (with icons) rebuilt only when it changes.
void Start() noexcept;
void Stop() noexcept;
[[nodiscard]] std::shared_ptr<const std::vector<Pack>> Current() noexcept;

}
