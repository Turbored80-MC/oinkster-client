#include "Oinkster/UserResourcePacks.hpp"
#include "Oinkster/CrosshairTexture.hpp"
#include "Oinkster/Diagnostics.hpp"
#include "Oinkster/Json.hpp"
#include "Oinkster/Modules/AccessibilityLogic.hpp"
#include "Oinkster/Modules/HudFormat.hpp"
#include <Windows.h>
#include <objbase.h>
#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <cwctype>
#include <condition_variable>
#include <filesystem>
#include <format>
#include <fstream>
#include <set>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>

namespace Oinkster::UserResourcePacks {
namespace {

namespace fs = std::filesystem;

constexpr std::uintmax_t MaxListBytes = 256 * 1024;
constexpr std::uintmax_t MaxManifestBytes = 256 * 1024;
constexpr std::uintmax_t MaxLangBytes = 1024 * 1024;
constexpr std::uintmax_t MaxIconBytes = 4 * 1024 * 1024;
constexpr std::size_t MaxPackFolders = 4096;
constexpr std::size_t MaxPacks = 64;
// Icons are drawn as solid cells by the HUD, so keep them small.
constexpr std::uint32_t IconEdge = 40;
constexpr auto PollInterval = std::chrono::seconds(2);

std::string Lowered(std::string_view text) {
    std::string lowered(text);
    std::ranges::transform(lowered, lowered.begin(), [](char character) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    });
    return lowered;
}

bool IsUuid(std::string_view text) {
    if (text.size() != 36) return false;
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (index == 8 || index == 13 || index == 18 || index == 23) {
            if (text[index] != '-') return false;
        } else if (!std::isxdigit(static_cast<unsigned char>(text[index]))) {
            return false;
        }
    }
    return true;
}

std::optional<std::string> ReadText(const fs::path& path, std::uintmax_t limit) {
    std::error_code error;
    const auto size = fs::file_size(path, error);
    if (error || size == 0 || size > limit) return std::nullopt;
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return std::nullopt;
    std::string text(static_cast<std::size_t>(size), '\0');
    if (!stream.read(text.data(), static_cast<std::streamsize>(text.size()))) return std::nullopt;
    // A UTF-8 byte-order mark is common in hand-made packs.
    if (text.starts_with("\xEF\xBB\xBF")) text.erase(0, 3);
    return text;
}

std::optional<fs::path> EnvironmentPath(const wchar_t* name) {
    std::array<wchar_t, MAX_PATH * 2> buffer{};
    const DWORD length = GetEnvironmentVariableW(name, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) return std::nullopt;
    return fs::path(std::wstring(buffer.data(), length));
}

// Every com.mojang folder that may belong to the player.
std::vector<fs::path> GameRoots() {
    std::vector<fs::path> roots;
    std::error_code error;
    if (const auto roaming = EnvironmentPath(L"APPDATA")) {
        for (fs::directory_iterator user(*roaming / L"Minecraft Bedrock" / L"Users", error), end; !error && user != end; user.increment(error)) {
            roots.push_back(user->path() / L"games" / L"com.mojang");
        }
    }
    if (const auto local = EnvironmentPath(L"LOCALAPPDATA")) {
        roots.push_back(*local / L"Packages" / L"Microsoft.MinecraftUWP_8wekyb3d8bbwe" / L"LocalState" / L"games" / L"com.mojang");
    }
    return roots;
}

struct ListFile {
    fs::path path;
    fs::file_time_type written{};
    std::uintmax_t size{};
    bool operator==(const ListFile&) const = default;
};

// The newest global_resource_packs.json: the one the game last wrote.
std::optional<ListFile> FindListFile(const std::vector<fs::path>& roots) {
    std::optional<ListFile> newest;
    for (const auto& root : roots) {
        const auto path = root / L"minecraftpe" / L"global_resource_packs.json";
        std::error_code error;
        const auto written = fs::last_write_time(path, error);
        if (error) continue;
        const auto size = fs::file_size(path, error);
        if (error) continue;
        if (!newest || written > newest->written) newest = ListFile{path, written, size};
    }
    return newest;
}

struct Folder {
    fs::path path;
    ManifestHeader header;
};

// uuid -> pack folder across the player's pack folders, one and two levels deep.
std::unordered_map<std::string, Folder> IndexFolders(const std::vector<fs::path>& roots) {
    std::unordered_map<std::string, Folder> index;
    std::size_t visited = 0;
    const auto consider = [&](const fs::path& folder) {
        const auto manifest = ReadText(folder / L"manifest.json", MaxManifestBytes);
        if (!manifest) return false;
        if (auto header = ParseManifestHeader(*manifest)) {
            auto uuid = header->uuid;
            index.try_emplace(std::move(uuid), Folder{folder, std::move(*header)});
        }
        return true;
    };
    for (const auto& root : roots) {
        for (const wchar_t* container : {L"resource_packs", L"development_resource_packs"}) {
            std::error_code error;
            for (fs::directory_iterator entry(root / container, error), end; !error && entry != end; entry.increment(error)) {
                if (++visited > MaxPackFolders) return index;
                std::error_code kind;
                if (!entry->is_directory(kind) || consider(entry->path())) continue;
                std::error_code nestedError;
                for (fs::directory_iterator nested(entry->path(), nestedError), nestedEnd; !nestedError && nested != nestedEnd;
                    nested.increment(nestedError)) {
                    if (++visited > MaxPackFolders) return index;
                    if (nested->is_directory(kind)) consider(nested->path());
                }
            }
        }
    }
    return index;
}

Pack BuildPack(const Folder& folder, bool withIcon) {
    Pack pack;
    pack.uuid = folder.header.uuid;
    pack.folder = folder.path;
    pack.version = Modules::FromUtf8(folder.header.version);
    // Only key-shaped text ("pack.name") needs the lang file.
    const auto keyShaped = [](const std::string& text) {
        return !text.empty() && text.find(' ') == std::string::npos && text.find('.') != std::string::npos;
    };
    std::string lang;
    const auto& raw = folder.header.name;
    if (keyShaped(raw) || keyShaped(folder.header.description)) {
        lang = ReadText(folder.path / L"texts" / L"en_US.lang", MaxLangBytes).value_or(std::string{});
    }
    pack.name = DisplayName(raw, lang, folder.path.filename().wstring());
    std::string description = folder.header.description;
    if (keyShaped(description) && !lang.empty()) description = LookupLangValue(lang, description).value_or(std::string{});
    pack.author = PackAuthor(folder.header.authors, description, Modules::ToUtf8(pack.name));
    if (withIcon) {
        std::ifstream stream(folder.path / L"pack_icon.png", std::ios::binary);
        std::error_code error;
        const auto size = fs::file_size(folder.path / L"pack_icon.png", error);
        if (stream && !error && size != 0 && size <= MaxIconBytes) {
            std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
            if (stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) {
                if (auto image = CrosshairTexture::DecodePng(bytes, IconEdge)) {
                    pack.icon = std::make_shared<const ArgbImage>(std::move(*image));
                }
            }
        }
    }
    return pack;
}

std::vector<Pack> ReadActive(const ListFile& file, const std::vector<fs::path>& roots) {
    const auto text = ReadText(file.path, MaxListBytes);
    if (!text) return {};
    const auto ids = ParseGlobalPackList(*text);
    if (ids.empty()) return {};
    const auto index = IndexFolders(roots);
    std::vector<Pack> packs;
    for (const auto& id : ids) {
        const auto found = index.find(id);
        if (found == index.end() || packs.size() >= MaxPacks) continue;
        packs.push_back(BuildPack(found->second, true));
    }
    return packs;
}

std::mutex mutex;
std::condition_variable wake;
// Explicit lifetime, as in CrosshairTexture: no process-exit destructor for
// a joinable std::thread to abort in.
std::thread* worker{};
bool stopping{};
std::shared_ptr<const std::vector<Pack>> current;

void WorkerMain() {
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::optional<ListFile> seen;
    bool first = true;
    for (;;) {
        {
            std::unique_lock lock(mutex);
            if (!first) wake.wait_for(lock, PollInterval, [] { return stopping; });
            if (stopping) break;
        }
        try {
            const auto roots = GameRoots();
            const auto file = FindListFile(roots);
            if (!first && file == seen) continue;
            first = false;
            seen = file;
            auto packs = std::make_shared<std::vector<Pack>>(file ? ReadActive(*file, roots) : std::vector<Pack>{});
            std::string sample = std::to_string(packs->size());
            for (const auto& pack : *packs) sample += std::format(" | {}{}", Modules::ToUtf8(pack.name), pack.icon ? "" : " (no icon)");
            Diagnostics::LogLine("info", "user_resource_packs", sample);
            std::scoped_lock lock(mutex);
            current = std::move(packs);
        } catch (...) {
            first = false;
        }
    }
    if (SUCCEEDED(initialized)) CoUninitialize();
}

}

std::vector<std::string> ParseGlobalPackList(std::string_view json) {
    std::vector<std::string> ids;
    const auto parsed = Json::Parse(CrosshairTexture::StripJsonComments(json));
    const auto* array = parsed ? parsed->AsArray() : nullptr;
    if (!array) return ids;
    std::set<std::string> seen;
    for (const auto& source : *array) {
        const auto* id = source.Find("pack_id");
        const auto* text = id ? id->AsString() : nullptr;
        if (!text || !IsUuid(*text)) continue;
        auto uuid = Lowered(*text);
        if (seen.insert(uuid).second && ids.size() < MaxPacks) ids.push_back(std::move(uuid));
    }
    return ids;
}

std::optional<std::vector<StackEntry>> ParseGlobalPackEntries(std::string_view json) {
    const auto parsed = Json::Parse(CrosshairTexture::StripJsonComments(json));
    const auto* array = parsed ? parsed->AsArray() : nullptr;
    if (!array) return std::nullopt;
    std::vector<StackEntry> entries;
    std::set<std::string> seen;
    if (array->size() > MaxPacks) return std::nullopt;
    for (const auto& source : *array) {
        const auto* id = source.Find("pack_id");
        const auto* text = id ? id->AsString() : nullptr;
        if (!text || !IsUuid(*text)) return std::nullopt;
        auto uuid = Lowered(*text);
        if (!seen.insert(uuid).second) return std::nullopt;
        StackEntry entry;
        entry.uuid = std::move(uuid);
        entry.source = source;
        if (const auto* subpack = source.Find("subpack")) {
            const auto* value = subpack->AsString();
            if (!value || value->size() > 128 || std::ranges::any_of(*value, [](const char character) {
                return static_cast<unsigned char>(character) < 0x20;
            })) return std::nullopt;
            entry.subpack = *value;
        }
        if (const auto* version = source.Find("version")) {
            if (const auto* string = version->AsString()) entry.version = *string;
            else if (const auto* parts = version->AsArray()) {
                for (const auto& part : *parts) {
                    const auto* number = part.AsNumber();
                    if (!number || *number < 0 || *number > 1'000'000 || *number != static_cast<long long>(*number)) {
                        return std::nullopt;
                    }
                    if (!entry.version.empty()) entry.version += '.';
                    entry.version += std::to_string(static_cast<long long>(*number));
                }
            }
        }
        if (entry.version.empty()) return std::nullopt;
        entries.push_back(std::move(entry));
    }
    return entries;
}

std::string SerializeGlobalPackEntries(std::span<const StackEntry> entries) {
    auto values = std::make_shared<Json::Array>();
    values->reserve(entries.size());
    for (const auto& entry : entries) values->push_back(entry.source);
    return Json::Stringify(Json::Value{std::move(values)}) + "\n";
}

std::optional<ManifestHeader> ParseManifestHeader(std::string_view manifest) {
    const auto parsed = Json::Parse(CrosshairTexture::StripJsonComments(manifest));
    const auto* header = parsed ? parsed->Find("header") : nullptr;
    const auto* uuid = header ? header->Find("uuid") : nullptr;
    const auto* uuidText = uuid ? uuid->AsString() : nullptr;
    if (!uuidText || !IsUuid(*uuidText)) return std::nullopt;
    ManifestHeader result{Lowered(*uuidText), {}, {}};
    if (const auto* name = header->Find("name"); name && name->AsString()) result.name = *name->AsString();
    if (const auto* description = header->Find("description"); description && description->AsString()) {
        result.description = *description->AsString();
    }
    const auto* metadata = parsed->Find("metadata");
    if (const auto* authors = metadata ? metadata->Find("authors") : nullptr; authors && authors->AsArray()) {
        for (const auto& author : *authors->AsArray()) {
            if (const auto* text = author.AsString(); text && !text->empty() && result.authors.size() < 8) result.authors.push_back(*text);
        }
    }
    if (const auto* version = header->Find("version")) {
        if (const auto* text = version->AsString()) {
            result.version = *text;
        } else if (const auto* parts = version->AsArray()) {
            for (const auto& part : *parts) {
                const auto* number = part.AsNumber();
                if (!number || *number < 0 || *number > 1'000'000) { result.version.clear(); break; }
                if (!result.version.empty()) result.version += '.';
                result.version += std::to_string(static_cast<long long>(*number));
            }
        }
    }
    return result;
}

std::optional<std::string> LookupLangValue(std::string_view lang, std::string_view key) {
    while (!lang.empty()) {
        const auto newline = lang.find('\n');
        std::string_view line = lang.substr(0, newline);
        lang = newline == std::string_view::npos ? std::string_view{} : lang.substr(newline + 1);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (!line.starts_with(key) || line.size() <= key.size() || line[key.size()] != '=') continue;
        std::string_view value = line.substr(key.size() + 1);
        // Trailing "\t## comment" is part of the .lang syntax.
        if (const auto comment = value.find("\t#"); comment != std::string_view::npos) value = value.substr(0, comment);
        while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.remove_suffix(1);
        return std::string(value);
    }
    return std::nullopt;
}

std::wstring DisplayName(std::string_view rawName, std::string_view lang, std::wstring_view fallback) {
    std::string resolved(rawName);
    if (!lang.empty()) {
        if (auto value = LookupLangValue(lang, rawName)) resolved = std::move(*value);
    }
    std::wstring name = Modules::StripFormattingCodes(Modules::FromUtf8(resolved));
    const auto isSpace = [](wchar_t character) { return character == L' ' || character == L'\t' || character == L'\r' || character == L'\n'; };
    while (!name.empty() && isSpace(name.back())) name.pop_back();
    std::size_t start = 0;
    while (start < name.size() && isSpace(name[start])) ++start;
    name.erase(0, start);
    // An unresolved lang key reads worse than the folder name.
    if (name.empty() || (name == Modules::FromUtf8(rawName) && !lang.empty())) return std::wstring(fallback);
    return name;
}

namespace {

std::wstring TrimAuthor(std::wstring text) {
    const auto isTrim = [](wchar_t character) {
        return character == L' ' || character == L'\t' || character == L'\r' || character == L'\n' || character == L'.'
            || character == L'-' || character == L':' || character == L'~' || character == L'"' || character == L'\'';
    };
    while (!text.empty() && isTrim(text.back())) text.pop_back();
    std::size_t start = 0;
    while (start < text.size() && (isTrim(text[start]) || text[start] == L'@')) ++start;
    return text.substr(start);
}

// The name after the first "by " that starts a word, up to the end of that
// phrase; empty when there is none or it reads like a sentence.
std::wstring CreditAfterBy(std::wstring_view text) {
    for (std::size_t at = 0; at + 3 <= text.size(); ++at) {
        if (std::towlower(text[at]) != L'b' || std::towlower(text[at + 1]) != L'y' || !std::iswspace(text[at + 2])) continue;
        if (at != 0 && !std::iswspace(text[at - 1]) && text[at - 1] != L'(' && text[at - 1] != L'[') continue;
        std::size_t end = at + 3;
        while (end < text.size() && std::wstring_view(L"\n|,()[];!").find(text[end]) == std::wstring_view::npos
            && !(text[end] == L'.' && (end + 1 == text.size() || std::iswspace(text[end + 1])))) {
            ++end;
        }
        auto credit = TrimAuthor(std::wstring(text.substr(at + 3, end - at - 3)));
        const auto words = 1 + std::ranges::count_if(credit, [](wchar_t character) { return std::iswspace(character) != 0; });
        if (!credit.empty() && words <= 3 && credit.size() <= 32) return credit;
    }
    return {};
}

}

std::wstring PackAuthor(const std::vector<std::string>& authors, std::string_view description, std::string_view name) {
    std::wstring joined;
    for (const auto& author : authors) {
        auto cleaned = TrimAuthor(Modules::StripFormattingCodes(Modules::FromUtf8(author)));
        if (cleaned.empty() || cleaned.size() > 32) continue;
        if (!joined.empty()) joined += L", ";
        joined += cleaned;
    }
    if (!joined.empty()) return joined;
    if (auto credit = CreditAfterBy(Modules::StripFormattingCodes(Modules::FromUtf8(description))); !credit.empty()) return credit;
    return CreditAfterBy(Modules::StripFormattingCodes(Modules::FromUtf8(name)));
}

std::vector<Pack> Installed() noexcept {
    try {
        const auto index = IndexFolders(GameRoots());
        std::vector<Pack> packs;
        packs.reserve(index.size());
        for (const auto& [uuid, folder] : index) packs.push_back(BuildPack(folder, false));
        std::ranges::sort(packs, [](const Pack& left, const Pack& right) {
            return left.name != right.name ? left.name < right.name : left.uuid < right.uuid;
        });
        return packs;
    } catch (...) {
        return {};
    }
}

Snapshot LoadSnapshot() noexcept {
    Snapshot snapshot;
    try {
        const auto roots = GameRoots();
        const auto file = FindListFile(roots);
        if (file) {
            snapshot.path = file->path;
            snapshot.written = file->written;
            snapshot.size = file->size;
            const auto text = ReadText(file->path, MaxListBytes);
            if (!text) {
                snapshot.error = L"Global Resources could not be read.";
                return snapshot;
            }
            snapshot.sourceJson = *text;
        } else {
            const auto root = std::ranges::find_if(roots, [](const fs::path& candidate) {
                std::error_code error;
                return fs::exists(candidate, error);
            });
            if (root == roots.end()) {
                snapshot.error = L"Minecraft's resource-pack folder was not found.";
                return snapshot;
            }
            snapshot.path = *root / L"minecraftpe" / L"global_resource_packs.json";
            snapshot.sourceJson = "[]";
        }
        const auto active = ParseGlobalPackEntries(snapshot.sourceJson);
        if (!active) {
            snapshot.error = L"global_resource_packs.json is malformed.";
            return snapshot;
        }
        snapshot.active = *active;
        const auto index = IndexFolders(roots);
        snapshot.packs.reserve(index.size());
        std::unordered_map<std::string, std::size_t> priorities;
        for (std::size_t i = 0; i < snapshot.active.size(); ++i) priorities.emplace(snapshot.active[i].uuid, i);
        for (const auto& [uuid, folder] : index) {
            auto pack = BuildPack(folder, true);
            if (const auto found = priorities.find(uuid); found != priorities.end()) {
                pack.active = true;
                pack.priority = found->second;
                pack.subpack = snapshot.active[found->second].subpack;
            }
            snapshot.packs.push_back(std::move(pack));
        }
        std::ranges::sort(snapshot.packs, [](const Pack& left, const Pack& right) {
            if (left.active != right.active) return left.active > right.active;
            if (left.active && left.priority != right.priority) return left.priority < right.priority;
            return left.name != right.name ? left.name < right.name : left.uuid < right.uuid;
        });
    } catch (...) {
        snapshot.error = L"Global Resources could not be loaded.";
    }
    return snapshot;
}

namespace {
bool SameSnapshot(const Snapshot& expected) {
    std::error_code error;
    if (!fs::exists(expected.path, error)) return !error && expected.size == 0 && expected.sourceJson == "[]";
    const auto written = fs::last_write_time(expected.path, error);
    if (error || written != expected.written) return false;
    const auto size = fs::file_size(expected.path, error);
    if (error || size != expected.size) return false;
    const auto text = ReadText(expected.path, MaxListBytes);
    return text && *text == expected.sourceJson;
}

Json::Value NewStackSource(const Pack& pack) {
    auto object = std::make_shared<Json::Object>();
    (*object)["pack_id"] = Json::Value{pack.uuid};
    if (!pack.subpack.empty()) (*object)["subpack"] = Json::Value{pack.subpack};
    auto version = std::make_shared<Json::Array>();
    const std::string versionText = Modules::ToUtf8(pack.version);
    std::string_view remaining = versionText;
    while (!remaining.empty()) {
        const auto dot = remaining.find('.');
        const auto part = remaining.substr(0, dot);
        long long number{};
        const auto parsed = std::from_chars(part.data(), part.data() + part.size(), number);
        if (parsed.ec != std::errc{} || parsed.ptr != part.data() + part.size() || number < 0 || number > 1'000'000) {
            version->clear();
            break;
        }
        version->push_back(Json::Value{static_cast<double>(number)});
        if (dot == std::string_view::npos) break;
        remaining.remove_prefix(dot + 1);
    }
    if (!version->empty()) (*object)["version"] = Json::Value{std::move(version)};
    return Json::Value{std::move(object)};
}
}

ApplyResult ApplyStack(const Snapshot& expected, std::span<const std::string> orderedActiveUuids,
    const ReloadCallback& reload) noexcept {
    if (!reload) return {ApplyStatus::ReloadUnavailable, L"Live resource reload is unavailable for this Minecraft build."};
    try {
        if (!expected.Ready()) return {ApplyStatus::Invalid, L"The resource-pack snapshot is not editable."};
        if (orderedActiveUuids.size() > MaxPacks) return {ApplyStatus::Invalid, L"At most 64 global resource packs can be active."};
        if (!SameSnapshot(expected)) return {ApplyStatus::Conflict, L"Minecraft changed Global Resources. Revert and try again."};

        std::unordered_map<std::string, const Pack*> installed;
        for (const auto& pack : expected.packs) installed.emplace(pack.uuid, &pack);
        std::unordered_map<std::string, const StackEntry*> original;
        for (const auto& entry : expected.active) original.emplace(entry.uuid, &entry);
        std::set<std::string> seen;
        std::vector<StackEntry> next;
        next.reserve(orderedActiveUuids.size());
        for (const auto& rawUuid : orderedActiveUuids) {
            const auto uuid = Lowered(rawUuid);
            const auto pack = installed.find(uuid);
            if (!IsUuid(uuid) || !seen.insert(uuid).second || pack == installed.end()) {
                return {ApplyStatus::Invalid, L"The selected pack stack is invalid."};
            }
            if (const auto found = original.find(uuid); found != original.end()) next.push_back(*found->second);
            else next.push_back(StackEntry{uuid, pack->second->subpack, Modules::ToUtf8(pack->second->version), NewStackSource(*pack->second)});
        }
        const std::string serialized = SerializeGlobalPackEntries(next);
        fs::create_directories(expected.path.parent_path());
        auto temporary = expected.path;
        temporary += L".oinkster.partial";
        auto backup = expected.path;
        backup += L".oinkster.backup";
        {
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            output.write(serialized.data(), static_cast<std::streamsize>(serialized.size()));
            output.flush();
            if (!output.good()) return {ApplyStatus::WriteFailed, L"The new pack stack could not be written."};
        }
        BOOL replaced = FALSE;
        if (expected.size == 0 && expected.sourceJson == "[]") {
            replaced = MoveFileExW(temporary.c_str(), expected.path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
            if (replaced) {
                std::ofstream prior(backup, std::ios::binary | std::ios::trunc);
                prior << "[]\n";
            }
        } else {
            replaced = ReplaceFileW(expected.path.c_str(), temporary.c_str(), backup.c_str(), REPLACEFILE_WRITE_THROUGH, nullptr, nullptr);
        }
        if (!replaced) {
            std::error_code ignored;
            fs::remove(temporary, ignored);
            return {ApplyStatus::WriteFailed, L"Global Resources could not be replaced."};
        }
        std::vector<std::string> ids;
        ids.reserve(next.size());
        for (const auto& entry : next) ids.push_back(entry.uuid);
        std::wstring reloadError;
        if (reload(ids, reloadError)) {
            std::error_code ignored;
            fs::remove(backup, ignored);
            return {ApplyStatus::Applied, L"Texture packs reloaded."};
        }
        const BOOL rolledBack = ReplaceFileW(expected.path.c_str(), backup.c_str(), nullptr, REPLACEFILE_WRITE_THROUGH, nullptr, nullptr);
        if (!rolledBack) return {ApplyStatus::RollbackFailed, L"Reload failed and the previous pack file could not be restored."};
        std::wstring ignoredReload;
        (void)reload(ParseGlobalPackList(expected.sourceJson), ignoredReload);
        return {ApplyStatus::ReloadFailed, reloadError.empty() ? L"Minecraft could not reload the selected packs." : std::move(reloadError)};
    } catch (...) {
        return {ApplyStatus::WriteFailed, L"Global Resources could not be updated."};
    }
}

void Start() noexcept {
    std::scoped_lock lock(mutex);
    if (worker) return;
    stopping = false;
    current.reset();
    try { worker = new std::thread(WorkerMain); } catch (...) {
        worker = nullptr;
        Diagnostics::LogLine("warn", "user_resource_packs", "the pack watcher could not start");
    }
}

void Stop() noexcept {
    std::thread* stoppingWorker{};
    {
        std::scoped_lock lock(mutex);
        stopping = true;
        stoppingWorker = std::exchange(worker, nullptr);
    }
    wake.notify_all();
    if (stoppingWorker) {
        if (stoppingWorker->joinable()) stoppingWorker->join();
        delete stoppingWorker;
    }
    std::scoped_lock lock(mutex);
    current.reset();
}

std::shared_ptr<const std::vector<Pack>> Current() noexcept {
    std::scoped_lock lock(mutex);
    return current;
}

}
