#include "Oinkster/Modules/Factories.hpp"
#include "Oinkster/Modules/HiveLogic.hpp"
#include "Oinkster/Modules/HudFormat.hpp"
#include "Oinkster/Modules/PacketLogic.hpp"
#include "Oinkster/Diagnostics.hpp"
#include "Oinkster/PacketSender.hpp"
#include "Oinkster/ReplayInjector.hpp"
#include <Windows.h>
#include <shellapi.h>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <format>
#include <fstream>

// Modules built on the server's own packets (ReplayPacketBridge): the player
// list, server helpers and skins. Packets are only copied for the ids an
// enabled module asks for.
namespace Oinkster::Modules {
namespace {

template<typename... Mixins>
class PacketModule : public ModuleBase<IModule>, public Mixins... {
public:
    using ModuleBase<IModule>::ModuleBase;
};

std::string LowerAscii(std::string value) {
    std::ranges::transform(value, value.begin(), [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
    return value;
}

// Server NPCs (The Hive's hub characters, shop displays) are listed with
// XUID "0" or none.
bool IsRealPlayer(const ListedPlayer& player) noexcept {
    return !player.xuid.empty() && std::ranges::any_of(player.xuid, [](char c) { return c != '0'; });
}

class PlayerListModule final : public PacketModule<IPacketObserverModule, IPlayerListModule> {
public:
    PlayerListModule(ModuleMetadata metadata, HudLayout, SharedServices&)
        : PacketModule(std::move(metadata), {
            KeySetting("key", "Show key", VK_TAB),
            ChoiceSetting("mode", "Activation", "hold", {{"hold", "Hold"}, {"toggle", "Toggle"}}),
            BoolSetting("heads", "Player heads", true),
            BoolSetting("hide-npcs", "Hide server NPCs", true, "Hides list entries without an Xbox account (hub characters)."),
            ChoiceSetting("sort", "Order", "name", {{"name", "Alphabetical"}, {"server", "Server order"}}),
            IntSetting("columns", "Columns", 0, 0, 6, "%d", "0 picks a column count from the player count."),
            FloatSetting("scale", "Scale", 1.0, 0.6, 2.0, "%.2fx")}) {}
    void OnSettingsChanged(const SettingsReader& settings) override {
        std::scoped_lock lock(mutex_);
        key_ = settings.Key("key");
        toggleMode_ = settings.Choice("mode") == "toggle";
        heads_ = settings.Bool("heads");
        hideNpcs_ = settings.Bool("hide-npcs");
        sortByName_ = settings.Choice("sort") == "name";
        columns_ = settings.Int("columns");
        scale_ = settings.Float("scale");
        toggled_ = false;
    }
    void OnDisable() override {
        std::scoped_lock lock(mutex_);
        roster_.Clear();
        faces_.clear();
        title_.clear();
        session_ = 0;
        toggled_ = held_ = gameplayVisible_ = false;
    }
    [[nodiscard]] bool WantsPacket(std::uint32_t packetId) const noexcept override { return packetId == GamePacket::PlayerList; }
    void OnReplayPacket(const ReplayPacketEvent& event) override {
        const auto packet = SplitPacket(std::span(reinterpret_cast<const std::uint8_t*>(event.bytes.data()), event.bytes.size()));
        std::scoped_lock lock(mutex_);
        if (event.session != session_) {
            session_ = event.session;
            roster_.Clear();
            faces_.clear();
            title_.clear();
            toggled_ = held_ = false;
        }
        if (!packet || packet->id != GamePacket::PlayerList) return;
        auto update = ParsePlayerList(packet->body);
        if (!update) return;
        if (update->remove) {
            for (const auto& player : update->players) faces_.erase(Key(player.uuid));
        } else {
            for (const auto& player : update->players) {
                if (auto face = HeadFace(player.skinWidth, player.skinHeight, player.skin)) faces_[Key(player.uuid)] = *face;
            }
        }
        roster_.Apply(std::move(*update));
    }
    void OnServerState(const ServerState& state) override {
        std::scoped_lock lock(mutex_);
        if (state.kind == SessionKind::None) {
            roster_.Clear();
            faces_.clear();
            title_.clear();
            session_ = 0;
            toggled_ = held_ = gameplayVisible_ = false;
            return;
        }
        title_ = ToUtf8(state.displayName);
    }
    void OnKeyboard(const KeyboardEvent& event) override {
        std::scoped_lock lock(mutex_);
        if (key_ == 0 || event.key != key_ || event.uiCaptured) return;
        if (toggleMode_ && event.pressed && !event.repeated) toggled_ = !toggled_;
    }
    void OnRender(const RenderEvent& event) override {
        std::scoped_lock lock(mutex_);
        gameplayVisible_ = event.gameplayVisible;
        held_ = event.gameplayVisible && !toggleMode_ && key_ != 0 && key_ < 256
            && event.keysDown && event.keysDown->test(key_);
    }
    [[nodiscard]] PlayerListView PlayerList() const override {
        std::scoped_lock lock(mutex_);
        PlayerListView view;
        view.visible = gameplayVisible_ && (toggleMode_ ? toggled_ : held_);
        if (!view.visible) return view;
        view.title = title_;
        view.heads = heads_;
        view.columns = columns_;
        view.scale = scale_;
        for (const auto& player : roster_.Players()) {
            if (hideNpcs_ && !IsRealPlayer(player)) continue;
            PlayerListRow row;
            row.name = ReplayInjector::CleanPlayerName(player.name);
            if (row.name.empty()) continue;
            if (const auto face = faces_.find(Key(player.uuid)); face != faces_.end()) {
                row.face = face->second;
                row.hasFace = true;
            }
            view.rows.push_back(std::move(row));
        }
        if (sortByName_) {
            std::ranges::sort(view.rows, {}, [](const PlayerListRow& row) { return LowerAscii(row.name); });
        }
        return view;
    }

private:
    static std::string Key(const std::array<std::uint8_t, 16>& uuid) { return std::string(uuid.begin(), uuid.end()); }

    PlayerRoster roster_;
    std::unordered_map<std::string, std::array<std::uint32_t, 64>> faces_;
    std::string title_;
    std::uint32_t session_{};
    std::uint32_t key_{VK_TAB};
    int columns_{};
    float scale_{1.0F};
    bool toggleMode_{};
    bool toggled_{};
    bool held_{};
    bool gameplayVisible_{};
    bool heads_{true};
    bool hideNpcs_{true};
    bool sortByName_{true};
};

// Copies UTF-8 text to the Windows clipboard.
void CopyToClipboard(std::string_view text) noexcept {
    const auto wide = FromUtf8(text);
    if (!OpenClipboard(nullptr)) return;
    EmptyClipboard();
    if (HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, (wide.size() + 1) * sizeof(wchar_t))) {
        if (auto* target = static_cast<wchar_t*>(GlobalLock(memory))) {
            std::memcpy(target, wide.c_str(), (wide.size() + 1) * sizeof(wchar_t));
            GlobalUnlock(memory);
            if (!SetClipboardData(CF_UNICODETEXT, memory)) GlobalFree(memory);
        } else {
            GlobalFree(memory);
        }
    }
    CloseClipboard();
}

std::optional<std::string> QuotedPlayer(std::string_view name) {
    if (name.empty() || name.size() > 32) return std::nullopt;
    std::string safe;
    safe.reserve(name.size() + 2);
    safe.push_back('"');
    for (const unsigned char character : name) {
        if (!(std::isalnum(character) || character == ' ' || character == '_' || character == '-')) return std::nullopt;
        safe.push_back(static_cast<char>(character));
    }
    safe.push_back('"');
    return safe;
}

// The Hive helpers: re-queue when a round ends, accept party/friend
// invites, vote for preferred maps and copy custom-server join codes. Only
// ever active while connected to The Hive. Commands go out through
// PacketSender with the CommandRequest layout learned from the player's own
// commands, so until one command has been typed this build, nothing is sent.
class HiveUtilsModule final : public HudModuleBase<IPacketObserverModule, INoticeModule> {
public:
    HiveUtilsModule(ModuleMetadata metadata, HudLayout layout, SharedServices&)
        : HudModuleBase(std::move(metadata), {
            BoolSetting("requeue", "Auto re-queue", true, "Queues the same game again when your round ends."),
            BoolSetting("requeue-death", "Re-queue on death (solo games)", false),
            BoolSetting("requeue-eliminated", "Re-queue when your team is eliminated", true),
            BoolSetting("hub-instead", "Go to the hub instead", false),
            IntSetting("delay-ms", "Re-queue delay", 500, 0, 5000, "%d ms"),
            KeySetting("requeue-key", "Re-queue now key", 0),
            BoolSetting("detect-game", "Detect the current game", true, "Runs /connection after each world change to learn which game to re-queue."),
            BoolSetting("party-accept", "Accept party invites", false),
            BoolSetting("friend-accept", "Accept friend invites", false),
            BoolSetting("map-vote", "Vote for preferred maps", false),
            ShownWhen(TextSetting("maps", "Preferred maps", "", "Comma-separated, best first."), "map-vote", {"true"}),
            ShownWhen(TextSetting("avoid", "Maps to avoid", "", "Comma-separated; picks any other map."), "map-vote", {"true"}),
            BoolSetting("copy-join-code", "Copy custom-server join codes", false)}, layout) {}
    void OnSettingsChanged(const SettingsReader& settings) override {
        std::scoped_lock lock(mutex_);
        requeue_ = settings.Bool("requeue");
        requeueDeath_ = settings.Bool("requeue-death");
        requeueEliminated_ = settings.Bool("requeue-eliminated");
        hubInstead_ = settings.Bool("hub-instead");
        delay_ = std::chrono::milliseconds(settings.Int("delay-ms"));
        requeueKey_ = settings.Key("requeue-key");
        detectGame_ = settings.Bool("detect-game");
        partyAccept_ = settings.Bool("party-accept");
        friendAccept_ = settings.Bool("friend-accept");
        mapVote_ = settings.Bool("map-vote");
        preferred_ = ParseMapList(settings.Text("maps"));
        avoided_ = ParseMapList(settings.Text("avoid"));
        copyJoinCode_ = settings.Bool("copy-join-code");
    }
    void OnDisable() override {
        std::scoped_lock lock(mutex_);
        onHive_ = false;
        awaitingConnection_ = false;
        requeueAt_.reset();
        game_.clear();
        team_.clear();
        lastJoinCode_.clear();
        notices_.clear();
    }
    [[nodiscard]] bool WantsPacket(std::uint32_t id) const noexcept override {
        return id == GamePacket::Text || id == GamePacket::SetTitle || id == GamePacket::ModalFormRequest
            || id == GamePacket::ChangeDimension;
    }
    void OnServerState(const ServerState& state) override {
        std::scoped_lock lock(mutex_);
        const bool hive = state.network == ServerNetwork::Hive;
        if (hive != onHive_) {
            onHive_ = hive;
            game_.clear();
            team_.clear();
            requeueAt_.reset();
        }
    }
    void OnReplayPacket(const ReplayPacketEvent& event) override {
        const auto packet = SplitPacket(std::span(reinterpret_cast<const std::uint8_t*>(event.bytes.data()), event.bytes.size()));
        if (!packet) return;
        std::scoped_lock lock(mutex_);
        if (!onHive_) return;
        switch (packet->id) {
        case GamePacket::ChangeDimension: {
            // Each game or hub is a fresh world; ask which one it is.
            team_.clear();
            if (detectGame_ && PacketSender::QueueCommand("/connection")) awaitingConnection_ = true;
            break;
        }
        case GamePacket::Text:
            if (const auto text = ParseText(packet->body)) OnHiveEvent(ClassifyHiveChat(text->message), event.capturedAt);
            break;
        case GamePacket::SetTitle:
            if (const auto title = ParseTitle(packet->body)) OnHiveEvent(ClassifyHiveTitle(title->text), event.capturedAt);
            break;
        case GamePacket::ModalFormRequest:
            if (mapVote_) VoteForMap(packet->body);
            break;
        default:
            break;
        }
    }
    void OnKeyboard(const KeyboardEvent& event) override {
        if (!event.pressed || event.repeated || event.uiCaptured) return;
        std::scoped_lock lock(mutex_);
        if (requeueKey_ != 0 && event.key == requeueKey_ && onHive_) Requeue();
    }
    void OnRender(const RenderEvent& event) override {
        std::scoped_lock lock(mutex_);
        NoteRender(event);
        if (requeueAt_ && event.presentedAt >= *requeueAt_) {
            requeueAt_.reset();
            Requeue();
        }
    }
    [[nodiscard]] std::vector<std::string> TakeNotices() override {
        std::scoped_lock lock(mutex_);
        return std::exchange(notices_, {});
    }
    [[nodiscard]] HudContent Content() const override {
        std::scoped_lock lock(mutex_);
        HudContent content;
        if (!onHive_) return content;
        content.lines.push_back({game_.empty() ? std::string("Hive") : "Hive  " + game_, HudTone::Accent});
        if (!PacketSender::CanSendCommands()) {
            content.lines.push_back({"Type any command once (e.g. /hub)", HudTone::Warning});
            content.lines.push_back({"to enable auto actions", HudTone::Warning});
        }
        return content;
    }

private:
    void Notice(std::string text) {
        if (notices_.size() < 8) notices_.push_back(std::move(text));
    }
    void ScheduleRequeue(SteadyTime at) {
        if (!requeue_ || game_.empty() || IsHiveHub(game_)) return;
        requeueAt_ = at + delay_;
    }
    void Requeue() {
        const std::string command = hubInstead_ || game_.empty() || IsHiveHub(game_) ? std::string("/hub") : "/q " + game_;
        if (PacketSender::QueueCommand(command)) Notice(command == "/hub" ? std::string("Back to the hub") : "Re-queued " + game_);
        else Notice("Type any command once so Oinkster can send commands");
    }
    void OnHiveEvent(const HiveEvent& event, SteadyTime at) {
        switch (event.kind) {
        case HiveEventKind::ConnectedServer:
            game_ = event.value;
            awaitingConnection_ = false;
            break;
        case HiveEventKind::GameOver:
        case HiveEventKind::Finished:
            ScheduleRequeue(at);
            break;
        case HiveEventKind::Died:
            if (requeueDeath_) ScheduleRequeue(at);
            break;
        case HiveEventKind::TeamAssigned:
            team_ = event.value;
            break;
        case HiveEventKind::TeamEliminated:
            if (requeueEliminated_ && !team_.empty() && event.value.find(team_) != std::string::npos) ScheduleRequeue(at);
            break;
        case HiveEventKind::PartyInvite:
            if (partyAccept_) {
                const auto player = QuotedPlayer(event.value);
                if (player && PacketSender::QueueCommand("/p accept " + *player)) Notice("Joined " + event.value + "'s party");
            }
            break;
        case HiveEventKind::FriendInvite:
            if (friendAccept_) {
                const auto player = QuotedPlayer(event.value);
                if (player && PacketSender::QueueCommand("/f accept " + *player)) Notice("Accepted " + event.value + "'s friend invite");
            }
            break;
        case HiveEventKind::JoinCode:
            if (copyJoinCode_ && event.value != lastJoinCode_) {
                lastJoinCode_ = event.value;
                CopyToClipboard(event.value);
                Notice("Copied join code " + event.value);
            }
            break;
        case HiveEventKind::None:
            break;
        }
    }
    void VoteForMap(std::span<const std::uint8_t> body) {
        const auto form = ParseFormRequest(body);
        if (!form) return;
        const auto buttons = MapVoteButtons(form->json);
        if (!buttons) return;
        const auto choice = ChooseMapButton(*buttons, preferred_, avoided_);
        if (!choice) return;
        if (PacketSender::QueueFormResponse(form->id, std::to_string(*choice))) Notice("Voted for " + (*buttons)[*choice]);
        else Notice("Answer one form yourself so Oinkster can vote");
    }

    std::vector<std::string> notices_;
    std::vector<std::string> preferred_;
    std::vector<std::string> avoided_;
    std::string game_;
    std::string team_;
    std::string lastJoinCode_;
    std::optional<SteadyTime> requeueAt_;
    std::chrono::milliseconds delay_{500};
    std::uint32_t requeueKey_{};
    bool onHive_{};
    bool awaitingConnection_{};
    bool requeue_{true};
    bool requeueDeath_{};
    bool requeueEliminated_{true};
    bool hubInstead_{};
    bool detectGame_{true};
    bool partyAccept_{};
    bool friendAccept_{};
    bool mapVote_{};
    bool copyJoinCode_{};
};

std::filesystem::path SkinFolder() {
    wchar_t localAppData[MAX_PATH]{};
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return {};
    return std::filesystem::path(localAppData) / "Oinkster" / "Client" / "skins";
}

// Skin grabber: saves the skin (and cape) of the player you look at - or
// any player by name - as PNG files, and can wear it: the server's own
// serialized copy of that skin is sent back in a PlayerSkin packet for the
// local player, exactly as the game does when you change skin.
class SkinGrabberModule final : public PacketModule<IPacketObserverModule, INoticeModule> {
public:
    SkinGrabberModule(ModuleMetadata metadata, HudLayout, SharedServices&)
        : PacketModule(std::move(metadata), {
            KeySetting("key", "Grab key", 0, "Grabs the player under your crosshair."),
            ChoiceSetting("on-key", "Grab key does", "save", {{"save", "Save the skin"}, {"wear", "Wear the skin"}, {"both", "Save and wear"}}),
            TextSetting("player", "Player name", "", "For the by-name actions below."),
            BoolSetting("cape", "Also save capes", true)}) {}
    void OnSettingsChanged(const SettingsReader& settings) override {
        std::scoped_lock lock(mutex_);
        key_ = settings.Key("key");
        const auto mode = settings.Choice("on-key");
        keySaves_ = mode != "wear";
        keyWears_ = mode != "save";
        named_ = settings.Text("player");
        saveCape_ = settings.Bool("cape");
    }
    void OnDisable() override {
        std::scoped_lock lock(mutex_);
        roster_.Clear();
        tracker_.Clear();
        ownSkin_.clear();
        notices_.clear();
        localUniqueId_.reset();
        session_ = 0;
        eye_ = {};
        yaw_ = pitch_ = 0.0F;
    }
    [[nodiscard]] std::vector<ModuleAction> Actions() const override {
        return {{"save-looked", "Save looked-at skin"}, {"wear-looked", "Wear looked-at skin"},
            {"save-named", "Save named player's skin"}, {"wear-named", "Wear named player's skin"},
            {"open-folder", "Open skins folder"}, {"restore", "Wear my own skin"}};
    }
    bool InvokeAction(std::string_view id, SteadyTime) override {
        std::scoped_lock lock(mutex_);
        if (id == "open-folder") {
            const auto folder = SkinFolder();
            std::error_code error;
            std::filesystem::create_directories(folder, error);
            ShellExecuteW(nullptr, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            return true;
        }
        if (id == "restore") {
            Wear(nullptr);
            return true;
        }
        const ListedPlayer* target = id.ends_with("looked") ? LookedAt() : roster_.Find(named_);
        if (!target) {
            Notice(id.ends_with("looked") ? "Look at a player first" : "No player named " + named_ + " is listed");
            return true;
        }
        if (id.starts_with("save")) Save(*target);
        else Wear(target);
        return true;
    }
    [[nodiscard]] bool WantsPacket(std::uint32_t id) const noexcept override {
        return id == GamePacket::PlayerList || id == GamePacket::StartGame || id == GamePacket::AddPlayer
            || id == GamePacket::MovePlayer || id == GamePacket::MoveActorAbsolute || id == GamePacket::MoveActorDelta;
    }
    void OnReplayPacket(const ReplayPacketEvent& event) override {
        const std::span bytes(reinterpret_cast<const std::uint8_t*>(event.bytes.data()), event.bytes.size());
        const auto packet = SplitPacket(bytes);
        if (!packet) return;
        std::scoped_lock lock(mutex_);
        if (event.session != session_) {
            session_ = event.session;
            roster_.Clear();
            tracker_.Clear();
            ownSkin_.clear();
            localUniqueId_.reset();
            eye_ = {};
            yaw_ = pitch_ = 0.0F;
        }
        if (packet->id == GamePacket::StartGame) {
            localUniqueId_ = ParseStartGameUniqueId(packet->body);
            return;
        }
        if (packet->id == GamePacket::PlayerList) {
            if (auto update = ParsePlayerList(packet->body)) {
                roster_.Apply(std::move(*update));
                // The first copy of our own skin the server sends is the one to restore.
                if (const auto* self = Self(); self && ownSkin_.empty()) ownSkin_ = self->serializedSkin;
            }
        }
        tracker_.Observe(bytes, ReplayInjector::Clock::now());
    }
    void OnPlayerTransform(const PlayerTransform& transform) override {
        std::scoped_lock lock(mutex_);
        eye_ = transform.position;
        if (transform.yawDegrees) yaw_ = *transform.yawDegrees;
        if (transform.pitchDegrees) pitch_ = *transform.pitchDegrees;
    }
    void OnKeyboard(const KeyboardEvent& event) override {
        if (!event.pressed || event.repeated || event.uiCaptured) return;
        std::scoped_lock lock(mutex_);
        if (key_ == 0 || event.key != key_) return;
        const auto* target = LookedAt();
        if (!target) {
            Notice("Look at a player first");
            return;
        }
        if (keySaves_) Save(*target);
        if (keyWears_) Wear(target);
    }
    [[nodiscard]] std::vector<std::string> TakeNotices() override {
        std::scoped_lock lock(mutex_);
        return std::exchange(notices_, {});
    }

private:
    void Notice(std::string text) {
        if (notices_.size() < 8) notices_.push_back(std::move(text));
    }
    [[nodiscard]] const ListedPlayer* Self() const {
        return localUniqueId_ ? roster_.FindByUniqueId(*localUniqueId_) : nullptr;
    }
    [[nodiscard]] const ListedPlayer* LookedAt() const {
        const auto players = tracker_.Players();
        const auto hit = ReplayInjector::PlayerAlongRay(players, eye_, yaw_, pitch_);
        return hit ? roster_.Find(hit->name) : nullptr;
    }
    void Save(const ListedPlayer& player) {
        const auto name = ReplayInjector::CleanPlayerName(player.name);
        if (player.skin.empty()) {
            Notice(name + " has no classic skin image to save");
            return;
        }
        const auto folder = SkinFolder();
        if (folder.empty()) {
            Notice("Could not locate the skins folder");
            return;
        }
        std::error_code error;
        std::filesystem::create_directories(folder, error);
        if (error) {
            Notice("Could not create the skins folder");
            return;
        }
        const auto write = [&](const std::filesystem::path& path, std::uint32_t width, std::uint32_t height, const std::vector<std::uint8_t>& rgba) {
            const auto png = EncodePng(width, height, rgba);
            if (png.empty()) return false;
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            output.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
            return output.good();
        };
        const auto base = SanitizeSkinFileName(name);
        if (!write(folder / (base + L".png"), player.skinWidth, player.skinHeight, player.skin)) {
            Notice("Could not save " + name + "'s skin");
            return;
        }
        if (saveCape_ && !player.cape.empty()
            && !write(folder / (base + L"-cape.png"), player.capeWidth, player.capeHeight, player.cape)) {
            Notice("Saved " + name + "'s skin, but could not save the cape");
            return;
        }
        Notice("Saved " + name + "'s skin");
    }
    // nullptr wears our own skin again.
    void Wear(const ListedPlayer* target) {
        const auto* self = Self();
        if (!self) {
            Notice("Rejoin the server so Oinkster sees your own player entry");
            return;
        }
        const auto& skin = target ? target->serializedSkin : ownSkin_;
        if (skin.empty()) {
            Notice(target ? "That skin did not decode completely; it can only be saved" : "Your own skin has not been seen yet");
            return;
        }
        const auto name = target ? ReplayInjector::CleanPlayerName(target->name) : std::string("your own");
        if (PacketSender::QueuePacket(BuildPlayerSkinPacket(self->uuid, skin, "", ""))) {
            Notice(target ? "Wearing " + name + "'s skin" : "Wearing your own skin");
        } else {
            Notice("Skins can be worn once a server connection validates");
        }
    }

    PlayerRoster roster_;
    ReplayInjector::PlayerTracker tracker_;
    std::vector<std::uint8_t> ownSkin_;
    std::vector<std::string> notices_;
    std::optional<std::int64_t> localUniqueId_;
    std::string named_;
    Vec3 eye_{};
    float yaw_{};
    float pitch_{};
    std::uint32_t session_{};
    std::uint32_t key_{};
    bool keySaves_{true};
    bool keyWears_{};
    bool saveCape_{true};
};
}

std::unique_ptr<IModule> MakePlayerList(ModuleMetadata m, HudLayout l, SharedServices& s) { return std::make_unique<PlayerListModule>(std::move(m), l, s); }

std::unique_ptr<IModule> MakeHiveUtils(ModuleMetadata m, HudLayout l, SharedServices& s) { return std::make_unique<HiveUtilsModule>(std::move(m), l, s); }
std::unique_ptr<IModule> MakeSkinGrabber(ModuleMetadata m, HudLayout l, SharedServices& s) { return std::make_unique<SkinGrabberModule>(std::move(m), l, s); }

}
