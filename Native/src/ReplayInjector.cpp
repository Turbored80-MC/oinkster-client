#include "Oinkster/ReplayInjector.hpp"

#include "Oinkster/Diagnostics.hpp"
#include "Oinkster/RakNet.hpp"
#include "Oinkster/ReplayCapture.hpp"
#include "Oinkster/ReplayFile.hpp"
#include "Oinkster/ReplayWorld.hpp"
#include "Oinkster/ScreenTelemetry.hpp"
#include "Oinkster/Modules/PvpLogic.hpp"
#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <deque>
#include <format>
#include <fstream>
#include <iterator>
#include <mutex>
#include <thread>

namespace Oinkster::ReplayInjector {
namespace {

// Recorded bytes pulled from the session per refill; the hook still hands
// them to the game one packet at a time.
constexpr std::size_t PumpBudgetBytes = 512U * 1024U;

namespace PacketId {
inline constexpr std::uint32_t AddPlayer = 12;
inline constexpr std::uint32_t PlayerList = 63;
inline constexpr std::uint32_t MoveActorAbsolute = 18;
inline constexpr std::uint32_t MovePlayer = 19;
inline constexpr std::uint32_t MoveActorDelta = 111;
inline constexpr std::uint32_t MobEquipment = 31;
inline constexpr std::uint32_t MobArmorEquipment = 32;
}
// For players who cannot be hidden: how far in front of their eyes the
// followed view sits, clear of their head.
constexpr double FaceClearance = 0.32;
constexpr float EyeHeight = 1.62F;
constexpr double PlayerWidth = 0.6;
constexpr double PlayerHeight = 1.8;
// How often a followed view is re-sent to the game (~60 Hz).
constexpr auto FollowInterval = std::chrono::milliseconds(16);
// Right-click picking: reach, and slack around a player's box so a moving
// target is easy to hit.
constexpr double PickReach = 96.0;
constexpr double PickSlack = 0.45;
constexpr std::size_t MaxListBytes = 48U * 1024U * 1024U;

float ReadF32(RakNet::Reader& reader) {
    const auto bytes = reader.Bytes(4);
    if (bytes.size() != 4) return 0.0F;
    const std::uint32_t value = static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8U)
        | (static_cast<std::uint32_t>(bytes[2]) << 16U) | (static_cast<std::uint32_t>(bytes[3]) << 24U);
    return std::bit_cast<float>(value);
}

bool Plausible(double value) { return std::isfinite(value) && std::abs(value) < 3.0e7; }

bool PlausiblePose(const Player& player) {
    return Plausible(player.eye.x) && Plausible(player.eye.y) && Plausible(player.eye.z)
        && Plausible(player.yaw) && Plausible(player.pitch);
}

// MoveActor packets send angles as one byte: 1/256ths of a full turn.
float ByteAngle(std::uint8_t value) { return static_cast<float>(value) * (360.0F / 256.0F); }

// MoveActorDelta's field flags (gophertunnel's layout).
constexpr std::uint16_t DeltaX = 0x01;
constexpr std::uint16_t DeltaY = 0x02;
constexpr std::uint16_t DeltaZ = 0x04;
constexpr std::uint16_t DeltaPitch = 0x08;
constexpr std::uint16_t DeltaYaw = 0x10;
constexpr std::uint16_t DeltaHeadYaw = 0x20;

float LerpAngle(float from, float to, float t) {
    float delta = std::fmod(to - from, 360.0F);
    if (delta > 180.0F) delta -= 360.0F;
    if (delta < -180.0F) delta += 360.0F;
    return from + delta * t;
}

// Disconnect without a message screen: the game leaves the world for the menu.
std::vector<std::uint8_t> DisconnectPacket() {
    RakNet::Writer writer;
    writer.VarU32(ReplayCapture::PacketId::Disconnect);
    writer.VarU32(0);  // reason: unknown (zigzag 0)
    writer.U8(1);      // skip the message
    return writer.Take();
}

const char* PhaseText(Phase phase) noexcept {
    switch (phase) {
    case Phase::Idle: return "";
    case Phase::Loading: return "Loading the replay...";
    case Phase::WaitingForWorld: return "Starting the replay world...";
    case Phase::JoiningWorld: return "Loading the replay world...";
    case Phase::Playing: return "Playing";
    case Phase::Finished: return "Finished";
    case Phase::Failed: return "Failed";
    }
    return "";
}

std::mutex mutex;
Controller controller;
// Bumped by every Watch/Stop so a load that finishes late is discarded.
std::uint64_t generation{};
std::jthread loader;
// Diagnostics: the last packets handed to the game, so a disconnect can be
// traced to what preceded it.
constexpr std::size_t RecentInjectedCapacity = 24;
std::deque<std::pair<std::uint32_t, long long>> recentInjected;
std::uint64_t injectedCount{};
Clock::time_point playingSince{};

std::string RecentInjectedText() {
    std::string text;
    for (const auto& [id, millis] : recentInjected) text += std::format("{}@{}ms ", id, millis);
    return text;
}

}

std::string CleanPlayerName(std::string_view raw) {
    std::string clean;
    for (std::size_t at = 0; at < raw.size();) {
        const auto byte = static_cast<std::uint8_t>(raw[at]);
        if (byte == '\n' || byte == '\r') break;
        std::size_t length = byte < 0x80 ? 1 : (byte & 0xE0) == 0xC0 ? 2 : (byte & 0xF0) == 0xE0 ? 3 : (byte & 0xF8) == 0xF0 ? 4 : 1;
        if (at + length > raw.size()) break;
        // Section sign (U+00A7, C2 A7): a formatting code and its character.
        if (length == 2 && byte == 0xC2 && static_cast<std::uint8_t>(raw[at + 1]) == 0xA7) {
            at += 2;
            if (at < raw.size()) {
                const auto next = static_cast<std::uint8_t>(raw[at]);
                at += next < 0x80 ? 1 : (next & 0xE0) == 0xC0 ? 2 : (next & 0xF0) == 0xE0 ? 3 : 4;
            }
            continue;
        }
        // U+E000-U+F8FF (EE 80 80 - EF A3 BF): server icon glyphs.
        const bool privateUse = length == 3 && (byte == 0xEE
            || (byte == 0xEF && static_cast<std::uint8_t>(raw[at + 1]) <= 0xA3));
        if (byte >= 0x20 && byte != 0x7F && !privateUse) clean.append(raw.substr(at, length));
        at += length;
    }
    const auto first = clean.find_first_not_of(' ');
    if (first == std::string::npos) return "Player";
    return clean.substr(first, clean.find_last_not_of(' ') - first + 1);
}

// Whether `lists` holds an entry for `uuid` with an XUID: the UUID, the
// unique id, the name, then the XUID as a decimal string.
bool ListedWithXuid(std::span<const std::uint8_t> lists, const std::array<std::uint8_t, 16>& uuid) {
    for (auto at = std::search(lists.begin(), lists.end(), uuid.begin(), uuid.end()); at != lists.end();
         at = std::search(at + 1, lists.end(), uuid.begin(), uuid.end())) {
        RakNet::Reader reader(lists.subspan(static_cast<std::size_t>(at - lists.begin()) + uuid.size()));
        (void)reader.VarU64();
        reader.Skip(reader.VarU32());
        const auto xuid = reader.Bytes(reader.VarU32());
        // NPCs get "0" (The Hive's hub characters and shop displays).
        const bool zero = std::ranges::all_of(xuid, [](std::uint8_t c) { return c == '0'; });
        if (reader.Ok() && !xuid.empty() && !zero && xuid.size() <= 20
            && std::ranges::all_of(xuid, [](std::uint8_t c) { return c >= '0' && c <= '9'; })) {
            return true;
        }
    }
    return false;
}

bool PlayerTracker::Listed(const std::array<std::uint8_t, 16>& uuid) const { return ListedWithXuid(lists_, uuid); }

void PlayerTracker::Observe(std::span<const std::uint8_t> packet, Clock::time_point now) {
    const auto header = ReplayCapture::ParseHeader(packet);
    if (!header) return;
    const auto body = packet.subspan(header->length);
    if (header->id == PacketId::PlayerList) {
        if (lists_.size() + body.size() <= MaxListBytes) lists_.insert(lists_.end(), body.begin(), body.end());
        for (auto& tracked : players_) {
            if (!tracked.listed) tracked.listed = ListedWithXuid(body, tracked.uuid);
        }
        return;
    }
    const auto byRuntimeId = [&](std::uint64_t runtimeId) {
        return std::ranges::find_if(players_, [&](const Tracked& tracked) { return tracked.current.runtimeId == runtimeId; });
    };
    RakNet::Reader reader(body);
    if (header->id == PacketId::MoveActorAbsolute || header->id == PacketId::MoveActorDelta) {
        // Servers move other players with these, not MovePlayer. Only players
        // this tracker saw added are followed; mobs and items are ignored.
        const auto found = byRuntimeId(reader.VarU64());
        if (!reader.Ok() || found == players_.end()) return;
        Player player = found->current;
        if (header->id == PacketId::MoveActorAbsolute) {
            // Flags, then the position (at eye height for players: an
            // AddPlayer at y 41.0 moves at 42.62), then pitch, body yaw and
            // head yaw as 1/256ths of a turn.
            (void)reader.U8();
            const float x = ReadF32(reader), y = ReadF32(reader), z = ReadF32(reader);
            player.eye = {x, y, z};
            player.pitch = ByteAngle(reader.U8());
            (void)reader.U8();
            player.yaw = ByteAngle(reader.U8());
        } else {
            // Only the fields that changed, each behind a flag.
            const auto flags = reader.U16LE();
            if (flags & DeltaX) player.eye.x = ReadF32(reader);
            if (flags & DeltaY) player.eye.y = ReadF32(reader);
            if (flags & DeltaZ) player.eye.z = ReadF32(reader);
            if (flags & DeltaPitch) player.pitch = ByteAngle(reader.U8());
            if (flags & DeltaYaw) (void)reader.U8();
            if (flags & DeltaHeadYaw) player.yaw = ByteAngle(reader.U8());
        }
        if (reader.Ok() && PlausiblePose(player)) MoveTo(*found, std::move(player), now);
        return;
    }
    if (header->id != PacketId::AddPlayer && header->id != PacketId::MovePlayer) return;
    Player player;
    std::array<std::uint8_t, 16> uuid{};
    // The view follows where a player's head points: head yaw, not body yaw.
    if (header->id == PacketId::AddPlayer) {
        const auto uuidBytes = reader.Bytes(16);
        if (uuidBytes.size() == 16) std::copy(uuidBytes.begin(), uuidBytes.end(), uuid.begin());
        const auto name = reader.Bytes(reader.VarU32());
        player.name = CleanPlayerName(std::string_view(reinterpret_cast<const char*>(name.data()), name.size()));
        player.runtimeId = reader.VarU64();
        reader.Skip(reader.VarU32());  // platform chat id
        const float x = ReadF32(reader), y = ReadF32(reader), z = ReadF32(reader);
        reader.Skip(12);  // velocity
        player.pitch = ReadF32(reader);
        (void)ReadF32(reader);  // body yaw
        player.yaw = ReadF32(reader);
        player.eye = {x, y + EyeHeight, z};
    } else {
        player.runtimeId = reader.VarU64();
        const float x = ReadF32(reader), y = ReadF32(reader), z = ReadF32(reader);
        player.pitch = ReadF32(reader);
        (void)ReadF32(reader);  // body yaw
        player.yaw = ReadF32(reader);
        player.eye = {x, y, z};  // MovePlayer positions are at eye height
    }
    if (!reader.Ok() || !PlausiblePose(player)) return;
    player.recorder = player.runtimeId == ReplayPlayback::RecorderRuntimeId;
    const auto found = byRuntimeId(player.runtimeId);
    if (found == players_.end()) {
        // A MovePlayer for a player this tracker never saw added is ignored.
        if (header->id != PacketId::AddPlayer || players_.size() >= 256) return;
        players_.push_back({player, player, now, now, uuid, player.recorder || Listed(uuid)});
        return;
    }
    if (header->id == PacketId::AddPlayer) {
        found->current = found->previous = player;
        found->currentAt = found->previousAt = now;
        found->uuid = uuid;
        found->listed = player.recorder || Listed(uuid);
        return;
    }
    player.name = found->current.name;
    MoveTo(*found, std::move(player), now);
}

void PlayerTracker::MoveTo(Tracked& tracked, Player player, Clock::time_point now) {
    // Ease from wherever the view is now to the new pose, arriving about as
    // far ahead as the last update was behind.
    const auto interval = std::min<Clock::duration>(now - tracked.previousAt, std::chrono::milliseconds(250));
    tracked.previous = PoseAt(player.runtimeId, now).value_or(tracked.current);
    tracked.previousAt = now;
    tracked.current = std::move(player);
    tracked.currentAt = now + interval;
}

std::vector<Player> PlayerTracker::Players() const {
    std::vector<Player> players;
    for (const auto& tracked : players_) {
        if (tracked.listed) players.push_back(tracked.current);
    }
    std::ranges::stable_sort(players, [](const Player& left, const Player& right) { return left.recorder && !right.recorder; });
    return players;
}

std::optional<Player> PlayerTracker::PoseAt(std::uint64_t runtimeId, Clock::time_point now) const {
    const auto found = std::ranges::find_if(players_, [&](const Tracked& tracked) { return tracked.current.runtimeId == runtimeId; });
    if (found == players_.end()) return std::nullopt;
    const auto span = std::chrono::duration<double>(found->currentAt - found->previousAt).count();
    if (span <= 0.0) return found->current;
    const double t = std::clamp(std::chrono::duration<double>(now - found->previousAt).count() / span, 0.0, 1.0);
    const auto lerp = [t](double from, double to) { return from + (to - from) * t; };
    Player pose = found->current;
    pose.eye = {lerp(found->previous.eye.x, found->current.eye.x), lerp(found->previous.eye.y, found->current.eye.y),
        lerp(found->previous.eye.z, found->current.eye.z)};
    pose.yaw = LerpAngle(found->previous.yaw, found->current.yaw, static_cast<float>(t));
    pose.pitch = static_cast<float>(lerp(found->previous.pitch, found->current.pitch));
    return pose;
}

std::vector<std::uint8_t> ViewerMovePacket(std::uint64_t viewerRuntimeId, const Vec3& eye, float yaw, float pitch) {
    RakNet::Writer writer;
    writer.VarU32(PacketId::MovePlayer);
    writer.VarU64(viewerRuntimeId);
    writer.F32LE(static_cast<float>(eye.x));
    writer.F32LE(static_cast<float>(eye.y));
    writer.F32LE(static_cast<float>(eye.z));
    writer.F32LE(pitch);
    writer.F32LE(yaw);
    writer.F32LE(yaw);   // head yaw
    // Mode teleport, as servers set a player's view (mode reset moved the
    // view and turned it, but left its pitch level).
    writer.U8(2);        // mode: teleport
    writer.U8(0);        // not on ground
    writer.VarU64(0);    // ridden entity
    writer.U8(1);        // teleport data (an optional on 1.26.50):
    writer.U32LE(0);     //   cause: unknown
    writer.U32LE(0);     //   source actor type
    writer.VarU64(0);    // tick
    return writer.Take();
}

Player BehindView(const Player& player) {
    constexpr double Back = 3.5;
    constexpr double Up = 0.8;
    const double yaw = static_cast<double>(player.yaw) * 3.14159265358979 / 180.0;
    // Bedrock yaw: 0 faces +Z (south), 90 faces -X (west).
    const double forwardX = -std::sin(yaw);
    const double forwardZ = std::cos(yaw);
    Player view = player;
    view.eye = {player.eye.x - forwardX * Back, player.eye.y + Up, player.eye.z - forwardZ * Back};
    view.pitch = 15.0F;
    return view;
}

std::optional<Player> PlayerAlongRay(std::span<const Player> players, const Vec3& eye, float yaw, float pitch) {
    const auto direction = Modules::LookDirection(yaw, pitch);
    std::optional<Player> best;
    double bestDistance = PickReach;
    for (const auto& player : players) {
        const double feet = player.eye.y - EyeHeight;
        const double half = PlayerWidth * 0.5 + PickSlack;
        const Aabb box{{player.eye.x - half, feet - PickSlack, player.eye.z - half},
            {player.eye.x + half, feet + PlayerHeight + PickSlack, player.eye.z + half}};
        const auto distance = Modules::RayEntryDistance(eye, direction, box);
        if (!distance || *distance > bestDistance) continue;
        bestDistance = *distance;
        best = player;
    }
    return best;
}

void Controller::BeginLoading() {
    Reset();
    phase_ = Phase::Loading;
}

void Controller::Loaded(ReplayPlayback::Session session) {
    session_ = std::move(session);
    pending_.clear();
    connection_ = 0;
    stackSeen_ = false;
    error_.clear();
    phase_ = Phase::WaitingForWorld;
}

void Controller::Fail(std::string error) {
    session_.reset();
    pending_.clear();
    connection_ = 0;
    error_ = std::move(error);
    phase_ = Phase::Failed;
}

void Controller::Reset() {
    session_.reset();
    pending_.clear();
    connection_ = 0;
    stackSeen_ = false;
    exiting_ = false;
    tracker_.Clear();
    spectating_.reset();
    cameraPath_.Clear();
    pathPlaying_ = false;
    ghostHidden_ = false;
    ghostEquipment_.clear();
    ghostArmor_.clear();
    error_.clear();
    phase_ = Phase::Idle;
}

Verdict Controller::OnServerPacket(std::uintptr_t connection, std::uint32_t id, bool sessionStart,
    bool localWorld, Clock::time_point now) {
    if (phase_ == Phase::WaitingForWorld) {
        // Never take over a connection to a real server.
        if (!sessionStart || !localWorld) return Verdict::Pass;
        connection_ = connection;
        stackSeen_ = false;
        phase_ = Phase::JoiningWorld;
        return Verdict::Pass;
    }
    const bool bound = phase_ == Phase::JoiningWorld || phase_ == Phase::Playing || phase_ == Phase::Finished;
    if (!bound) return Verdict::Pass;
    if (sessionStart) {
        // A new connection means the viewer left the replay world.
        Reset();
        return Verdict::Pass;
    }
    if (connection != connection_) return Verdict::Pass;
    if (phase_ == Phase::JoiningWorld) {
        // The real handshake, up to and including the pack stack, belongs to
        // the local world: the client must finish it with its own server.
        // Only its pack list and stack are the recorded server's, so the
        // server's resource packs load.
        if (!stackSeen_) {
            if (id == ReplayCapture::PacketId::ResourcePacksInfo && !session_->PacksInfoPacket().empty()) {
                pending_.push_back(session_->PacksInfoPacket());
                return Verdict::Drop;
            }
            stackSeen_ = id == ReplayCapture::PacketId::ResourcePackStack;
            if (stackSeen_ && !session_->PackStackPacket().empty()) {
                pending_.push_back(session_->PackStackPacket());
                return Verdict::Drop;
            }
            return Verdict::Pass;
        }
        session_->BeginInjected(now);
        phase_ = Phase::Playing;
    }
    return Verdict::Drop;
}

Verdict Controller::OnClientPacket(std::span<const std::uint8_t> packet) {
    // Once recorded packets own the client, its gameplay packets contain the
    // recorded server's actor ids and world state. Letting those reach the
    // throwaway local server makes it reject the client (server recordings do
    // this especially quickly). The local connection is only a transport for
    // replay packets from this point on.
    if (phase_ == Phase::Playing || phase_ == Phase::Finished) return Verdict::Drop;
    if (phase_ != Phase::JoiningWorld || !session_) return Verdict::Pass;
    std::vector<std::vector<std::uint8_t>> answers;
    if (!session_->AnswerPackDownload(packet, answers)) return Verdict::Pass;
    for (auto& answer : answers) pending_.push_back(std::move(answer));
    return Verdict::Drop;
}

std::optional<std::vector<std::uint8_t>> Controller::Next(std::uintptr_t connection, Clock::time_point now) {
    // While joining, only the swapped pack handshake is handed over.
    if (phase_ == Phase::JoiningWorld && connection == connection_ && connection_ != 0 && !pending_.empty()) {
        auto packet = std::move(pending_.front());
        pending_.pop_front();
        return packet;
    }
    if (connection != connection_ || !session_ || (phase_ != Phase::Playing && phase_ != Phase::Finished)) return std::nullopt;
    if (exiting_) {
        Reset();
        return DisconnectPacket();
    }
    // The camera path, like a followed view, goes ahead of the recording.
    if (pathPlaying_ && !spectating_ && now - lastFollow_ >= FollowInterval) {
        if (const auto pose = cameraPath_.Evaluate(session_->PositionMicros(now))) {
            pending_.push_front(ViewerMovePacket(session_->ViewerRuntimeId(), pose->eye, pose->yaw, pose->pitch));
        }
        lastFollow_ = now;
    }
    // A followed player's view goes ahead of the recording's own packets.
    if (spectating_ && now - lastFollow_ >= FollowInterval) {
        // Through the recorder's eyes, the recorded camera itself, sampled at
        // the playback position rather than chased packet by packet.
        const auto camera = *spectating_ == ReplayPlayback::RecorderRuntimeId
            ? session_->RecorderCameraAt(now) : std::nullopt;
        if (camera && camera->yaw && camera->pitch) {
            const Vec3 eye{camera->x, camera->y + EyeHeight, camera->z};
            pending_.push_front(ViewerMovePacket(session_->ViewerRuntimeId(), eye, *camera->yaw, *camera->pitch));
            lastFollow_ = now;
        } else if (const auto pose = tracker_.PoseAt(*spectating_, now)) {
            auto eye = pose->eye;
            if (!pose->recorder) {
                const auto look = Modules::LookDirection(pose->yaw, pose->pitch);
                eye = {eye.x + look.x * FaceClearance, eye.y + look.y * FaceClearance, eye.z + look.z * FaceClearance};
            }
            pending_.push_front(ViewerMovePacket(session_->ViewerRuntimeId(), eye, pose->yaw, pose->pitch));
            lastFollow_ = now;
        } else {
            spectating_.reset();
            ghostHidden_ = false;
        }
    }
    if (pending_.empty()) {
        std::vector<std::vector<std::uint8_t>> due;
        session_->Pump(now, PumpBudgetBytes, due);
        for (auto& packet : due) pending_.push_back(std::move(packet));
    }
    if (session_->CurrentStage() == ReplayPlayback::Stage::Finished) phase_ = Phase::Finished;
    if (pending_.empty()) return std::nullopt;
    auto packet = std::move(pending_.front());
    pending_.pop_front();
    tracker_.Observe(packet, now);
    if (const auto header = ReplayCapture::ParseHeader(packet);
        header && (header->id == PacketId::MobEquipment || header->id == PacketId::MobArmorEquipment)) {
        RakNet::Reader reader(std::span(packet).subspan(header->length));
        if (reader.VarU64() == ReplayPlayback::RecorderRuntimeId && reader.Ok()) {
            (header->id == PacketId::MobEquipment ? ghostEquipment_ : ghostArmor_) = packet;
        }
    }
    return packet;
}

void Controller::HideGhost() {
    if (ghostHidden_ || !session_) return;
    auto remove = session_->GhostRemovePacket();
    if (remove.empty()) return;
    pending_.push_front(std::move(remove));
    ghostHidden_ = true;
}

void Controller::ShowGhost(Clock::time_point now) {
    if (!ghostHidden_ || !session_) return;
    ghostHidden_ = false;
    const auto pose = tracker_.PoseAt(ReplayPlayback::RecorderRuntimeId, now);
    if (!pose) return;
    auto spawn = session_->GhostSpawnPacket(pose->eye.x, pose->eye.y, pose->eye.z, pose->yaw, pose->pitch);
    if (spawn.empty()) return;
    std::vector<std::vector<std::uint8_t>> packets{std::move(spawn)};
    if (!ghostEquipment_.empty()) packets.push_back(ghostEquipment_);
    if (!ghostArmor_.empty()) packets.push_back(ghostArmor_);
    pending_.insert(pending_.begin(), packets.begin(), packets.end());
}

bool Controller::TeleportTo(std::uint64_t runtimeId, Clock::time_point now) {
    if (!session_ || (phase_ != Phase::Playing && phase_ != Phase::Finished)) return false;
    const auto pose = tracker_.PoseAt(runtimeId, now);
    if (!pose) return false;
    Spectate(std::nullopt, now);
    const auto view = BehindView(*pose);
    pending_.push_front(ViewerMovePacket(session_->ViewerRuntimeId(), view.eye, view.yaw, view.pitch));
    return true;
}

void Controller::SetCameraPath(ReplayCameraPath::Path path) {
    cameraPath_ = std::move(path);
    if (cameraPath_.Empty()) pathPlaying_ = false;
}

void Controller::PlayCameraPath(bool playing, Clock::time_point now) {
    if (playing && (cameraPath_.Empty() || !session_)) return;
    if (playing && spectating_) Spectate(std::nullopt, now);
    pathPlaying_ = playing;
    lastFollow_ = {};
}

void Controller::Spectate(std::optional<std::uint64_t> runtimeId, Clock::time_point now) {
    if (runtimeId) pathPlaying_ = false;
    const bool recorder = runtimeId == ReplayPlayback::RecorderRuntimeId;
    if (!recorder) ShowGhost(now);
    spectating_ = runtimeId;
    // The first view follows a frame later, after the despawn below.
    lastFollow_ = now;
    if (recorder && session_ && (phase_ == Phase::Playing || phase_ == Phase::Finished)) HideGhost();
}

void Controller::Exit() {
    if (phase_ == Phase::Playing || phase_ == Phase::Finished) exiting_ = true;
    else Reset();
}

void Controller::SetPaused(bool paused, Clock::time_point now) {
    if (session_) session_->SetPaused(paused, now);
}

void Controller::SetSpeed(float speed, Clock::time_point now) {
    if (session_) session_->SetSpeed(speed, now);
}

void Controller::Seek(std::uint64_t positionMicros, Clock::time_point now) {
    if (!session_ || (phase_ != Phase::Playing && phase_ != Phase::Finished)) return;
    // A rewind replays from the start, so packets already pumped (from the
    // old timeline) would arrive twice.
    if (session_->SeekTo(positionMicros, now)) pending_.clear();
    phase_ = Phase::Playing;
}

Status Controller::Snapshot(Clock::time_point now) const {
    Status status;
    status.phase = phase_;
    status.running = phase_ != Phase::Idle && phase_ != Phase::Failed;
    status.ready = session_.has_value();
    status.inWorld = phase_ == Phase::JoiningWorld || phase_ == Phase::Playing || phase_ == Phase::Finished;
    status.playing = phase_ == Phase::Playing || phase_ == Phase::Finished;
    status.phaseText = PhaseText(phase_);
    status.error = error_;
    if (session_) {
        status.paused = session_->Paused();
        status.speed = session_->Speed();
        status.positionMicros = session_->PositionMicros(now);
        status.lengthMicros = session_->LengthMicros();
        status.marks = session_->Marks();
    }
    if (spectating_) {
        if (const auto pose = tracker_.PoseAt(*spectating_, now)) status.spectating = pose->name;
    }
    status.cameraKeyframes = cameraPath_.Size();
    status.cameraPathPlaying = pathPlaying_;
    return status;
}

std::filesystem::path CameraPathFile(const std::filesystem::path& replay) {
    auto file = replay;
    file.replace_extension(L".camera.json");
    return file;
}

namespace {
// The replay being watched, whose keyframes SetCameraPath saves.
std::filesystem::path watched;

std::optional<ReplayCameraPath::Path> LoadCameraPath(const std::filesystem::path& replay) {
    std::ifstream file(CameraPathFile(replay), std::ios::binary);
    if (!file) return std::nullopt;
    std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    return ReplayCameraPath::Path::FromJson(text);
}
}

bool Watch(const std::filesystem::path& replay, std::string& error) {
    try {
        std::uint64_t token{};
        {
            std::scoped_lock lock(mutex);
            controller.BeginLoading();
            token = ++generation;
        }
        loader = std::jthread([replay, token] {
            std::string loadError;
            std::optional<ReplayPlayback::Session> session;
            try {
                const auto read = Replay::Read(replay);
                if (!read.error.empty() && read.records.empty()) loadError = read.error;
                else session = ReplayPlayback::Session::Load(read, loadError);
            } catch (...) {
                loadError = "The replay could not be read.";
            }
            std::optional<ReplayCameraPath::Path> keyframes;
            try { keyframes = LoadCameraPath(replay); } catch (...) {}
            std::scoped_lock lock(mutex);
            if (token != generation) return;
            watched = replay;
            if (session) {
                // The recorded server's own blocks, as a behavior pack the
                // replay world loads (written before the world is started).
                std::string packError;
                const auto pack = session->CustomBlocks().empty()
                    ? std::vector<ReplayBlockPack::PackFile>{} : ReplayBlockPack::BuildPack(session->CustomBlocks());
                const auto folders = ReplayWorld::PrepareWorld(pack, packError);
                Diagnostics::LogLine(packError.empty() ? "info" : "warning", "replay_world_blocks", std::format(
                    "customBlocks={} worldFolders={}{}", session->CustomBlocks().size(), folders,
                    packError.empty() ? std::string() : " error=" + packError));
            }
            if (session) {
                Diagnostics::LogLine("info", "replay_inject_ready", std::format("packets={} recorderPlayer={} recorderSkin={} oldHeightChunks={}",
                    session->PacketCount(), session->HasGhost(), session->GhostHasSkin(), session->ModernizedChunks()));
                controller.Loaded(std::move(*session));
                if (keyframes) controller.SetCameraPath(std::move(*keyframes));
            } else {
                Diagnostics::LogLine("warning", "replay_inject_load", loadError);
                controller.Fail(loadError.empty() ? "The replay could not be loaded." : loadError);
            }
        });
        return true;
    } catch (...) {
        error = "The replay loader could not be started.";
        std::scoped_lock lock(mutex);
        controller.Fail(error);
        return false;
    }
}

void Stop() noexcept {
    std::scoped_lock lock(mutex);
    ++generation;
    controller.Reset();
}

void Shutdown() noexcept {
    Stop();
    try {
        if (loader.joinable()) loader.join();
    } catch (...) {}
}

bool Running() noexcept {
    std::scoped_lock lock(mutex);
    const auto phase = controller.CurrentPhase();
    return phase != Phase::Idle && phase != Phase::Failed;
}

Status CurrentStatus() {
    std::scoped_lock lock(mutex);
    return controller.Snapshot(Clock::now());
}

void SetPaused(bool paused) noexcept {
    try {
        std::scoped_lock lock(mutex);
        controller.SetPaused(paused, Clock::now());
    } catch (...) {}
}

void SetSpeed(float speed) noexcept {
    try {
        std::scoped_lock lock(mutex);
        controller.SetSpeed(speed, Clock::now());
    } catch (...) {}
}

void Seek(std::uint64_t positionMicros) noexcept {
    try {
        std::scoped_lock lock(mutex);
        controller.Seek(positionMicros, Clock::now());
    } catch (...) {}
}

std::vector<Player> Players() {
    std::scoped_lock lock(mutex);
    return controller.Players();
}

void TeleportTo(std::uint64_t runtimeId) noexcept {
    try {
        std::scoped_lock lock(mutex);
        const bool moved = controller.TeleportTo(runtimeId, Clock::now());
        Diagnostics::LogLine("info", "replay_teleport", std::format("runtimeId={} moved={}", runtimeId, moved));
    } catch (...) {}
}

void Spectate(std::optional<std::uint64_t> runtimeId) noexcept {
    try {
        std::scoped_lock lock(mutex);
        controller.Spectate(runtimeId, Clock::now());
        Diagnostics::LogLine("info", "replay_spectate", runtimeId ? std::format("following runtimeId={}", *runtimeId) : std::string("stopped"));
    } catch (...) {}
}

std::filesystem::path WatchedReplay() {
    std::scoped_lock lock(mutex);
    return controller.CurrentPhase() == Phase::Idle ? std::filesystem::path() : watched;
}

ReplayCameraPath::Path CameraPath() {
    std::scoped_lock lock(mutex);
    return controller.CameraPath();
}

void SetCameraPath(ReplayCameraPath::Path path) noexcept {
    try {
        std::filesystem::path file;
        std::string json;
        {
            std::scoped_lock lock(mutex);
            if (controller.CurrentPhase() == Phase::Idle) return;
            controller.SetCameraPath(path);
            if (!watched.empty()) file = CameraPathFile(watched);
            json = path.ToJson();
        }
        if (file.empty()) return;
        std::error_code ignored;
        if (path.Empty()) {
            std::filesystem::remove(file, ignored);
            return;
        }
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out.write(json.data(), static_cast<std::streamsize>(json.size()));
        if (!out) Diagnostics::LogLine("warning", "replay_camera_save", "could not save the camera keyframes");
    } catch (...) {}
}

void PlayCameraPath(bool playing) noexcept {
    try {
        std::scoped_lock lock(mutex);
        controller.PlayCameraPath(playing, Clock::now());
        Diagnostics::LogLine("info", "replay_camera_path", std::format("playing={} keyframes={}",
            controller.PlayingCameraPath(), controller.CameraPath().Size()));
    } catch (...) {}
}

void Exit() noexcept {
    try {
        std::scoped_lock lock(mutex);
        ++generation;
        controller.Exit();
        Diagnostics::LogLine("info", "replay_exit", "leaving the replay world");
    } catch (...) {}
}

Verdict OnServerPacket(std::uintptr_t connection, std::uint32_t id, bool sessionStart) noexcept {
    try {
        // Read outside the lock: it only touches an atomic.
        const bool localWorld = !ScreenTelemetry::Known() || ScreenTelemetry::LastLoadWasLocalWorld();
        std::scoped_lock lock(mutex);
        const auto before = controller.CurrentPhase();
        const auto verdict = controller.OnServerPacket(connection, id, sessionStart, localWorld, Clock::now());
        const auto after = controller.CurrentPhase();
        if (before != after) {
            if (after == Phase::Playing) {
                playingSince = Clock::now();
                recentInjected.clear();
                injectedCount = 0;
            }
            Diagnostics::LogLine("info", "replay_inject_phase", std::format("{} -> {} (packet {})",
                PhaseText(before), PhaseText(after), id));
            if (before == Phase::Playing || before == Phase::Finished) {
                Diagnostics::LogLine("info", "replay_inject_ended", std::format("injected={} after {}ms; last: {}",
                    injectedCount, std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - playingSince).count(),
                    RecentInjectedText()));
            }
        } else if (before == Phase::WaitingForWorld && sessionStart && !localWorld) {
            Diagnostics::LogLine("info", "replay_inject_skip", "not taking over a connection to an external server");
        }
        return verdict;
    } catch (...) {
        return Verdict::Pass;
    }
}

bool WantsClientPacket(std::uint32_t id) noexcept {
    return id == ReplayCapture::PacketId::ResourcePackClientResponse || id == 84;  // ResourcePackChunkRequest
}

Verdict OnClientPacket(std::span<const std::uint8_t> packet) noexcept {
    try {
        std::scoped_lock lock(mutex);
        const auto verdict = controller.OnClientPacket(packet);
        const auto header = ReplayCapture::ParseHeader(packet);
        if (header && header->id == ReplayCapture::PacketId::ResourcePackClientResponse
            && controller.CurrentPhase() == Phase::JoiningWorld) {
            RakNet::Reader reader(packet.subspan(header->length));
            const auto status = reader.VarU32();
            Diagnostics::LogLine("info", "replay_inject_packs", std::format("client pack response status={} answeredFromRecording={}",
                status, verdict == Verdict::Drop));
        }
        return verdict;
    } catch (...) {
        return Verdict::Pass;
    }
}

void NoteDroppedDisconnect(std::span<const std::uint8_t> packet) noexcept {
    try {
        std::string text;
        for (const auto byte : packet.subspan(std::min<std::size_t>(packet.size(), 1))) {
            if (byte >= 0x20 && byte < 0x7F) text.push_back(static_cast<char>(byte));
            else if (!text.empty() && text.back() != ' ') text.push_back(' ');
            if (text.size() >= 300) break;
        }
        std::scoped_lock lock(mutex);
        Diagnostics::LogLine("warning", "replay_inject_kick", std::format(
            "the local world's server sent Disconnect after {}ms (injected={}): \"{}\"; last injected: {}",
            std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - playingSince).count(),
            injectedCount, text, RecentInjectedText()));
    } catch (...) {}
}

bool NextPacket(std::uintptr_t connection, std::vector<std::uint8_t>& out) noexcept {
    try {
        std::scoped_lock lock(mutex);
        auto packet = controller.Next(connection, Clock::now());
        if (!packet) return false;
        if (const auto header = ReplayCapture::ParseHeader(*packet)) {
            const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - playingSince).count();
            recentInjected.emplace_back(header->id, millis);
            if (recentInjected.size() > RecentInjectedCapacity) recentInjected.pop_front();
            if (header->id == 12) {
                Diagnostics::LogLine("info", "replay_inject_addplayer", std::format("AddPlayer bytes={} at {}ms (#{})",
                    packet->size(), millis, injectedCount + 1));
            }
        }
        ++injectedCount;
        out = std::move(*packet);
        return true;
    } catch (...) {
        return false;
    }
}

}
