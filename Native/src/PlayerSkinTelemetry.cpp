#include "Oinkster/PlayerSkinTelemetry.hpp"
#include "Oinkster/PlayerSkinLayout.hpp"
#include "Oinkster/Dx12Overlay.hpp"
#include "Oinkster/Diagnostics.hpp"
#include "Oinkster/HookManager.hpp"
#include "Oinkster/SafeMemory.hpp"
#include "Oinkster/UnloadGuard.hpp"
#include <Windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <libhat/Scanner.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include "../resources/resource.h"

namespace Oinkster::PlayerSkinTelemetry {
namespace {
using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;
namespace Layout = PlayerSkinLayout;

constexpr std::uint64_t FallbackRevision = 0x4F494E4B53544552ULL;
constexpr std::size_t MaxCandidates = 32;
constexpr std::size_t ProbeSnapshotBytes = 0x1000;
constexpr std::size_t ProbeSnapshotSlots = ProbeSnapshotBytes / sizeof(std::uintptr_t);

std::atomic_bool attached{};
std::atomic_bool scanStopping{};
std::atomic_bool refreshRequested{};
// Live reads of game memory are gated on the ClickGUI being open. Polling the
// render Actor every second and walking the heap in the background caused
// periodic frame spikes during gameplay, and the skin is only visible in the
// menu anyway; the cached atlas covers everything before the first open.
std::atomic_bool menuOpen{};
std::atomic_bool renderActorRefreshPending{};
std::atomic_bool authoritativeDelivered{};
HANDLE scanWorker{};
std::size_t localPlayerVtableIndex{};
using UpdateSkinFn = void (*)(void*, const void*, int);
UpdateSkinFn originalUpdateSkin{};
void* updateSkinTarget{};
std::atomic_uint32_t activeUpdateSkinCallbacks{};
std::atomic_bool skinHookStopping{};
std::atomic_uint64_t deliveredRevision{FallbackRevision};
std::atomic<std::ptrdiff_t> resolvedPlayerMapOffset{-1};
Clock::time_point lastPoll{};
Clock::time_point lastDirectPlayerPoll{};
int resourceAnchor{};
bool probeEnabled{};

struct OwnershipDescriptor {
    std::uint64_t magic{};
    std::uint32_t version{};
    std::uint32_t imageSize{};
    std::uint32_t timeDateStamp{};
    std::uint32_t pointerKind{};
    std::uint64_t updateSkinRva{};
    std::uint64_t localPlayerVtableRva{};
    std::uint64_t retainedMemberOffset{};
};

constexpr std::uint64_t OwnershipMagic = 0x314E574F4E494B4FULL; // "OKINOWN1"
constexpr std::uint32_t OwnershipVersion = 2;
std::optional<OwnershipDescriptor> ownershipDescriptor;

struct ProbeObservation {
    void* player{};
    Layout::RetainedPointerIdentity identity{};
    std::uint64_t revision{};
    std::array<std::uintptr_t, ProbeSnapshotSlots> after{};
};

std::mutex probeMutex;
std::vector<ProbeObservation> probeSequence;

constexpr std::uint64_t SkinCacheMagic = 0x314E494B534B4E4FULL; // "ONKSKIN1"
// Version 1 could contain a heap false-positive selected through a stale
// mIsPrimaryUser offset. Do not ever revive one of those atlases.
constexpr std::uint32_t SkinCacheVersion = 2;

struct SkinCacheHeader {
    std::uint64_t magic{};
    std::uint32_t version{};
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t reserved{};
    std::uint64_t revision{};
    std::uint64_t byteCount{};
};

std::filesystem::path ClientDataDirectory() {
    wchar_t localAppData[MAX_PATH]{};
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH);
    const auto directory = length > 0 && length < MAX_PATH
        ? std::filesystem::path(localAppData) / "Oinkster" / "Client"
        : std::filesystem::temp_directory_path() / "Oinkster";
    return directory;
}

std::filesystem::path SkinCachePath() { return ClientDataDirectory() / "player-skin-cache.bin"; }
std::filesystem::path AnalysisDirectory() { return ClientDataDirectory() / "analysis"; }
std::filesystem::path OwnershipDescriptorPath() {
    return AnalysisDirectory() / "skin-ownership-1.26.5203.bin";
}

bool ProbeRequested() noexcept {
    wchar_t value[8]{};
    const DWORD length = GetEnvironmentVariableW(L"OINKSTER_SKIN_PROBE", value, 8);
    if (length > 0 && length < 8 && value[0] != L'0') return true;
    std::error_code error;
    return std::filesystem::exists(AnalysisDirectory() / "enable-skin-probe", error);
}

bool ReadModuleIdentity(std::uint32_t& imageSize, std::uint32_t& timestamp) noexcept {
    const auto* base = reinterpret_cast<const std::uint8_t*>(GetModuleHandleW(nullptr));
    IMAGE_DOS_HEADER dos{};
    if (!base || !SafeMemory::CopyGuarded(&dos, base, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE) return false;
    IMAGE_NT_HEADERS64 nt{};
    if (dos.e_lfanew <= 0 || !SafeMemory::CopyGuarded(&nt, base + dos.e_lfanew, sizeof(nt))
        || nt.Signature != IMAGE_NT_SIGNATURE) return false;
    imageSize = nt.OptionalHeader.SizeOfImage;
    timestamp = nt.FileHeader.TimeDateStamp;
    return imageSize != 0;
}

void SaveOwnershipDescriptor(const OwnershipDescriptor& descriptor) noexcept {
    try {
        const auto path = OwnershipDescriptorPath();
        std::filesystem::create_directories(path.parent_path());
        auto temporary = path;
        temporary += L".tmp";
        {
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            output.write(reinterpret_cast<const char*>(&descriptor), sizeof(descriptor));
            if (!output) return;
        }
        if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            return;
        }
        ownershipDescriptor = descriptor;
    } catch (...) {}
}

void LoadOwnershipDescriptor() noexcept {
    ownershipDescriptor.reset();
    try {
        OwnershipDescriptor descriptor{};
        std::ifstream input(OwnershipDescriptorPath(), std::ios::binary);
        input.read(reinterpret_cast<char*>(&descriptor), sizeof(descriptor));
        std::uint32_t imageSize{}, timestamp{};
        if (!input || descriptor.magic != OwnershipMagic || descriptor.version != OwnershipVersion
            || !ReadModuleIdentity(imageSize, timestamp) || descriptor.imageSize != imageSize
            || descriptor.timeDateStamp != timestamp
            || descriptor.updateSkinRva == 0
            || descriptor.retainedMemberOffset >= ProbeSnapshotBytes
            || descriptor.localPlayerVtableRva >= imageSize
            || (descriptor.pointerKind != static_cast<std::uint32_t>(Layout::RetainedPointerKind::ThreadOwner)
                && descriptor.pointerKind != static_cast<std::uint32_t>(Layout::RetainedPointerKind::PlayerSkin))) {
            return;
        }
        ownershipDescriptor = descriptor;
        Diagnostics::LogLine("info", "player_skin_descriptor", std::format(
            "validated 1.26.5203 ownership descriptor: vtableRva={:#x} memberOffset={:#x} kind={}",
            descriptor.localPlayerVtableRva, descriptor.retainedMemberOffset, descriptor.pointerKind));
    } catch (...) {}
}

void SaveSkinCache(std::uint32_t width, std::uint32_t height,
    std::span<const std::uint8_t> pixels, std::uint64_t revision) noexcept {
    try {
        const auto path = SkinCachePath();
        std::filesystem::create_directories(path.parent_path());
        auto temporary = path;
        temporary += L".tmp";
        const SkinCacheHeader header{SkinCacheMagic, SkinCacheVersion, width, height, 0,
            revision, static_cast<std::uint64_t>(pixels.size())};
        {
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            output.write(reinterpret_cast<const char*>(&header), sizeof(header));
            output.write(reinterpret_cast<const char*>(pixels.data()),
                static_cast<std::streamsize>(pixels.size()));
            if (!output) throw std::runtime_error("skin cache write failed");
        }
        if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            // Packaged Win32 processes can reject an atomic rename even when
            // ordinary file creation in the redirected LocalAppData folder is
            // allowed. Fall back to a direct replacement so a successfully
            // acquired skin survives the next launch instead of forcing a
            // multi-gigabyte heap scan every time.
            const DWORD moveError = GetLastError();
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            const SkinCacheHeader replacementHeader{SkinCacheMagic, SkinCacheVersion, width, height, 0,
                revision, static_cast<std::uint64_t>(pixels.size())};
            output.write(reinterpret_cast<const char*>(&replacementHeader), sizeof(replacementHeader));
            output.write(reinterpret_cast<const char*>(pixels.data()),
                static_cast<std::streamsize>(pixels.size()));
            output.close();
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            if (!output) {
                Diagnostics::LogLine("warn", "player_skin_cache", std::format(
                    "equipped atlas cache replacement failed: moveError={} path={}",
                    moveError, path.string()));
                return;
            }
            Diagnostics::LogLine("info", "player_skin_cache", std::format(
                "atomic cache replacement was unavailable (error={}); direct replacement succeeded",
                moveError));
        }
        Diagnostics::LogLine("info", "player_skin_cache", std::format(
            "equipped atlas cached: revision={:#x} atlas={}x{} bytes={}",
            revision, width, height, pixels.size()));
    } catch (...) {
        Diagnostics::LogLine("warn", "player_skin_cache", "equipped atlas could not be cached");
    }
}

bool LoadSkinCache() noexcept {
    try {
        std::ifstream input(SkinCachePath(), std::ios::binary);
        SkinCacheHeader header{};
        input.read(reinterpret_cast<char*>(&header), sizeof(header));
        if (!input || header.magic != SkinCacheMagic || header.version != SkinCacheVersion
            || !Layout::IsSupportedSkinExtent(header.width, header.height)
            || header.byteCount != Layout::ExpectedByteCount(header.width, header.height)
            || header.revision == 0 || header.revision == FallbackRevision) return false;
        std::vector<std::uint8_t> pixels(static_cast<std::size_t>(header.byteCount));
        input.read(reinterpret_cast<char*>(pixels.data()), static_cast<std::streamsize>(pixels.size()));
        if (!input || Layout::HashPixels(pixels) != header.revision) {
            Diagnostics::LogLine("warn", "player_skin_cache", "cached atlas failed integrity validation");
            return false;
        }
        if (!Layout::HasUsableHeadPixels(pixels, header.width, header.height)) {
            Diagnostics::LogLine("warn", "player_skin_cache",
                "cached atlas had no usable base-layer face; ignoring the false-positive cache");
            return false;
        }
        if (!Dx12Overlay::UpdatePlayerSkin(header.width, header.height, pixels, header.revision)) return false;
        deliveredRevision.store(header.revision, std::memory_order_release);
        Diagnostics::LogLine("info", "player_skin_cache", std::format(
            "cached equipped atlas loaded: revision={:#x} atlas={}x{}",
            header.revision, header.width, header.height));
        return true;
    } catch (...) {
        return false;
    }
}

// ---------------------------------------------------------------------------
// Rate-limited staged diagnostics.
//
// Every acquisition stage reports why it stopped, but ObserveClientInstance
// runs once a second for the whole session, so an unconditional log would bury
// the file. A reason is emitted immediately when it *changes* (that is the
// interesting event) and at most once every 15s while it persists.
// ---------------------------------------------------------------------------
constexpr auto StageLogInterval = std::chrono::seconds(15);

struct StageState {
    std::string event;
    std::string message;
    Clock::time_point at{};
};

std::mutex stageMutex;
std::vector<StageState> stageStates;

void StageLog(std::string_view level, std::string_view event, std::string message) noexcept {
    try {
        const auto now = Clock::now();
        bool emit = true;
        {
            const std::lock_guard<std::mutex> guard(stageMutex);
            const auto existing = std::find_if(stageStates.begin(), stageStates.end(),
                [&](const StageState& state) { return state.event == event; });
            if (existing == stageStates.end()) {
                stageStates.push_back(StageState{std::string(event), message, now});
            } else if (existing->message == message && now - existing->at < StageLogInterval) {
                emit = false;
            } else {
                existing->message = message;
                existing->at = now;
            }
        }
        if (emit) Diagnostics::LogLine(level, event, message);
    } catch (...) {
        // Diagnostics must never be able to take the acquisition path down.
    }
}

void ResetStageLog() noexcept {
    try {
        const std::lock_guard<std::mutex> guard(stageMutex);
        stageStates.clear();
    } catch (...) {}
}

// Guarded reads of the game's address space live in Oinkster/SafeMemory.hpp
// so WorldTelemetry's offset chain shares this exact implementation rather
// than re-deriving (or, as it previously did, skipping) the range check.
using SafeMemory::CopyGuarded;
using SafeMemory::IsReadable;
using SafeMemory::ReadField;

struct CompleteObjectLocator64 {
    std::uint32_t signature{};
    std::uint32_t offset{};
    std::uint32_t constructorDisplacement{};
    std::uint32_t typeDescriptorRva{};
    std::uint32_t classDescriptorRva{};
    std::uint32_t selfRva{};
};

std::optional<std::string> ResolveMsvcTypeName(const void* object) noexcept {
    if (!object) return std::nullopt;
    void** vtable{};
    const CompleteObjectLocator64* locator{};
    if (!ReadField(object, 0, vtable) || !vtable
        || !ReadField(vtable - 1, 0, locator)
        || !locator) return std::nullopt;

    CompleteObjectLocator64 value{};
    if (!CopyGuarded(&value, locator, sizeof(value)) || value.signature != 1 || value.selfRva == 0) {
        return std::nullopt;
    }
    const auto locatorAddress = reinterpret_cast<std::uintptr_t>(locator);
    if (locatorAddress < value.selfRva) return std::nullopt;
    const auto imageBase = locatorAddress - value.selfRva;
    if (imageBase != reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr))) return std::nullopt;
    const auto nameAddress = imageBase + value.typeDescriptorRva + 2 * sizeof(void*);
    std::array<char, 192> name{};
    for (std::size_t index = 0; index + 1 < name.size(); ++index) {
        if (!CopyGuarded(&name[index], reinterpret_cast<const void*>(nameAddress + index), 1)) return std::nullopt;
        if (name[index] == '\0') return std::string(name.data(), index);
    }
    return std::nullopt;
}

std::string ReadGameString(const void* value) noexcept {
    std::uint64_t size{}, capacity{};
    if (!ReadField(value, Layout::StringSizeOffset, size)
        || !ReadField(value, Layout::StringCapacityOffset, capacity)
        || !Layout::PlausibleStringFields(size, capacity, 256) || size == 0) return {};
    const char* characters{};
    if (capacity == Layout::StringSsoCapacity) {
        characters = static_cast<const char*>(value);
    } else if (!ReadField(value, Layout::StringBufferOffset, characters) || !characters) {
        return {};
    }
    std::string result(static_cast<std::size_t>(size), '\0');
    if (!CopyGuarded(result.data(), characters, result.size())) return {};
    for (char& character : result) {
        if (static_cast<unsigned char>(character) < 0x20) character = '?';
    }
    return result;
}

using ProbeSnapshot = std::array<std::uintptr_t, ProbeSnapshotSlots>;

ProbeSnapshot CapturePlayerSnapshot(const void* player) noexcept {
    ProbeSnapshot snapshot{};
    for (std::size_t index = 0; index < snapshot.size(); ++index) {
        (void)ReadField(player, index * sizeof(std::uintptr_t), snapshot[index]);
    }
    return snapshot;
}

std::string ChangedSnapshotOffsets(const ProbeSnapshot& before, const ProbeSnapshot& after) {
    std::string result;
    std::size_t count{};
    for (std::size_t index = 0; index < before.size(); ++index) {
        if (before[index] == after[index]) continue;
        if (count++ != 0) result += ',';
        result += std::format("{:#x}", index * sizeof(std::uintptr_t));
        if (count == 48) { result += ",..."; break; }
    }
    return result.empty() ? "none" : result;
}

std::string CaptureCallerRvas() noexcept {
    std::array<void*, 24> frames{};
    const USHORT count = RtlCaptureStackBackTrace(1, static_cast<ULONG>(frames.size()), frames.data(), nullptr);
    std::uint32_t imageSize{}, timestamp{};
    if (!ReadModuleIdentity(imageSize, timestamp)) return "unavailable";
    (void)timestamp;
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    std::string result;
    for (USHORT index = 0; index < count; ++index) {
        const auto address = reinterpret_cast<std::uintptr_t>(frames[index]);
        if (address < base || address >= base + imageSize) continue;
        if (!result.empty()) result += ',';
        result += std::format("{:#x}", address - base);
    }
    return result.empty() ? "none" : result;
}

void AppendRawProbeSnapshot(const ProbeSnapshot& before, const ProbeSnapshot& after,
    std::uintptr_t player, std::uint64_t revision) noexcept {
    if (!probeEnabled) return;
    try {
        const auto directory = AnalysisDirectory();
        std::filesystem::create_directories(directory);
        const auto path = directory / std::format("skin-probe-{}.bin", GetCurrentProcessId());
        std::ofstream output(path, std::ios::binary | std::ios::app);
        const std::array<std::uint64_t, 4> header{{0x3145424F52504B4FULL, player, revision, ProbeSnapshotBytes}};
        output.write(reinterpret_cast<const char*>(header.data()), sizeof(header));
        output.write(reinterpret_cast<const char*>(before.data()), sizeof(before));
        output.write(reinterpret_cast<const char*>(after.data()), sizeof(after));
    } catch (...) {}
}

// ---------------------------------------------------------------------------
// PlayerSkin validation (shared by both acquisition paths)
// ---------------------------------------------------------------------------

// Checks the five std::strings that open PlayerSkin. Their size/capacity
// fields are self-consistent in a way arbitrary memory is not, which is what
// lets the heap path tell a real PlayerSkin from a coincidental byte pattern.
bool SkinStringsPlausible(const void* skin) noexcept {
    constexpr std::size_t stringOffsets[] = {
        Layout::SkinIdOffset, Layout::SkinPlayFabIdOffset, Layout::SkinFullIdOffset,
        Layout::SkinResourcePatchOffset, Layout::SkinDefaultGeometryNameOffset};
    for (const auto offset : stringOffsets) {
        std::uint64_t size{};
        std::uint64_t capacity{};
        if (!ReadField(skin, offset + Layout::StringSizeOffset, size)) return false;
        if (!ReadField(skin, offset + Layout::StringCapacityOffset, capacity)) return false;
        if (!Layout::PlausibleStringFields(size, capacity)) return false;
    }
    return true;
}

struct SkinFlags {
    std::uint8_t trusted{};
    std::uint8_t premium{};
    std::uint8_t persona{};
    std::uint8_t personaCapeOnClassic{};
    std::uint8_t primaryUser{};
    std::uint8_t overridesAppearance{};
    std::uint8_t armSize{};
};

bool ReadSkinFlags(const void* skin, SkinFlags& flags) noexcept {
    return ReadField(skin, Layout::SkinTrustedFlagOffset, flags.trusted)
        && ReadField(skin, Layout::SkinPremiumOffset, flags.premium)
        && ReadField(skin, Layout::SkinPersonaOffset, flags.persona)
        && ReadField(skin, Layout::SkinPersonaCapeOnClassicOffset, flags.personaCapeOnClassic)
        && ReadField(skin, Layout::SkinPrimaryUserOffset, flags.primaryUser)
        && ReadField(skin, Layout::SkinOverridesAppearanceOffset, flags.overridesAppearance)
        && ReadField(skin, Layout::SkinArmSizeOffset, flags.armSize);
}

struct SkinImageHeader {
    std::int32_t format{};
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint64_t byteCount{};
    const std::uint8_t* pixels{};
};

bool ReadSkinImageHeader(const void* skin, SkinImageHeader& header) noexcept {
    const auto image = Layout::SkinImageOffset;
    return ReadField(skin, image + Layout::ImageFormatOffset, header.format)
        && ReadField(skin, image + Layout::ImageWidthOffset, header.width)
        && ReadField(skin, image + Layout::ImageHeightOffset, header.height)
        && ReadField(skin, image + Layout::ImageByteCountOffset, header.byteCount)
        && ReadField(skin, image + Layout::ImagePixelsOffset, header.pixels);
}

// Turns a validated PlayerSkin into a ranking candidate. `reason` is filled in
// on rejection so the caller can report *why* a plausible-looking skin was
// skipped rather than silently falling back.
bool BuildCandidate(const void* skin, bool fromPlayerMap, Layout::SkinCandidate& candidate,
    std::string_view& reason) noexcept {
    if (!IsReadable(skin, Layout::PlayerSkinSize)) { reason = "skin_unreadable"; return false; }

    SkinFlags flags{};
    if (!ReadSkinFlags(skin, flags)) { reason = "flags_unreadable"; return false; }
    if (!Layout::PlausibleSkinFlags(flags.trusted, flags.premium, flags.persona,
            flags.personaCapeOnClassic, flags.primaryUser, flags.overridesAppearance, flags.armSize)) {
        reason = "flag_block_implausible";
        return false;
    }

    SkinImageHeader header{};
    if (!ReadSkinImageHeader(skin, header)) { reason = "image_header_unreadable"; return false; }
    const auto verdict = Layout::ClassifySkinImage(header.format, header.width, header.height,
        header.byteCount, header.pixels != nullptr);
    if (verdict != Layout::ImageVerdict::Ok) { reason = Layout::Describe(verdict); return false; }
    if (!IsReadable(header.pixels, static_cast<std::size_t>(header.byteCount))) {
        reason = "pixels_unreadable";
        return false;
    }

    candidate.skinAddress = reinterpret_cast<std::uint64_t>(skin);
    candidate.pixelsAddress = reinterpret_cast<std::uint64_t>(header.pixels);
    candidate.width = header.width;
    candidate.height = header.height;
    candidate.primaryUser = flags.primaryUser != 0;
    candidate.persona = flags.persona != 0;
    candidate.fromPlayerMap = fromPlayerMap;
    candidate.imageFormat = header.format;
    reason = "ok";
    return true;
}

std::uint64_t CandidateRevision(const Layout::SkinCandidate& candidate) noexcept {
    const auto byteCount = static_cast<std::size_t>(
        Layout::ExpectedByteCount(candidate.width, candidate.height));
    std::vector<std::uint8_t> pixels;
    try { pixels.resize(byteCount); } catch (...) { return 0; }
    if (!CopyGuarded(pixels.data(), reinterpret_cast<const void*>(candidate.pixelsAddress), byteCount)) return 0;
    return Layout::HashPixels(pixels);
}

struct HookProbeContext {
    ProbeSnapshot before{};
    std::string typeName;
    std::string callers;
    bool localPlayer{};
};

HookProbeContext BeginHookProbe(void* player) noexcept {
    HookProbeContext context{};
    const auto type = ResolveMsvcTypeName(player);
    context.typeName = type.value_or("rtti_unavailable");
    context.localPlayer = type.has_value() && Layout::IsLocalPlayerTypeName(*type);
    if (probeEnabled) {
        context.before = CapturePlayerSnapshot(player);
        context.callers = CaptureCallerRvas();
        Diagnostics::LogLine(context.localPlayer ? "info" : "warn", "player_skin_probe_rtti", std::format(
            "player={:#x} type={} local={} thread={} callers={}", reinterpret_cast<std::uintptr_t>(player),
            context.typeName, context.localPlayer, GetCurrentThreadId(), context.callers));
    }
    return context;
}

void RecordProbeObservation(void* player, const void* skinReference, const void* owner,
    const Layout::SkinCandidate& candidate, std::uint64_t revision,
    const HookProbeContext& context) noexcept {
    if (!probeEnabled || !context.localPlayer || revision == 0) return;
    const auto after = CapturePlayerSnapshot(player);
    AppendRawProbeSnapshot(context.before, after, reinterpret_cast<std::uintptr_t>(player), revision);
    const auto skinId = ReadGameString(reinterpret_cast<const std::uint8_t*>(
        reinterpret_cast<const void*>(candidate.skinAddress)) + Layout::SkinIdOffset);
    Diagnostics::LogLine("info", "player_skin_probe_observation", std::format(
        "player={:#x} reference={:#x} owner={:#x} skin={:#x} skinId={} format={} atlas={}x{} "
        "revision={:#x} changedOffsets={}", reinterpret_cast<std::uintptr_t>(player),
        reinterpret_cast<std::uintptr_t>(skinReference), reinterpret_cast<std::uintptr_t>(owner),
        candidate.skinAddress, skinId.empty() ? "<empty>" : skinId, candidate.imageFormat,
        candidate.width, candidate.height, revision, ChangedSnapshotOffsets(context.before, after)));

    const std::lock_guard<std::mutex> guard(probeMutex);
    const Layout::RetainedPointerIdentity identity{
        reinterpret_cast<std::uintptr_t>(owner), static_cast<std::uintptr_t>(candidate.skinAddress)};
    if (!probeSequence.empty() && probeSequence.front().player != player) probeSequence.clear();
    if (!probeSequence.empty() && probeSequence.back().revision == revision) return;
    probeSequence.push_back(ProbeObservation{player, identity, revision, after});
    if (probeSequence.size() < 3) {
        Diagnostics::LogLine("info", "player_skin_probe_sequence", std::format(
            "captured step {} of A->B->A (revision={:#x})", probeSequence.size(), revision));
        return;
    }
    if (probeSequence.size() > 3) probeSequence.erase(probeSequence.begin());
    const auto& first = probeSequence[0];
    const auto& second = probeSequence[1];
    const auto& third = probeSequence[2];
    if (first.revision != third.revision || first.revision == second.revision) {
        Diagnostics::LogLine("warn", "player_skin_probe_sequence", std::format(
            "sequence is not A->B->A yet: {:#x}->{:#x}->{:#x}",
            first.revision, second.revision, third.revision));
        return;
    }
    const auto selected = Layout::SelectRetainedSkinOffset(first.after, second.after, third.after,
        first.identity, second.identity, third.identity);
    if (selected.status != Layout::RetainedOffsetStatus::Unique) {
        Diagnostics::LogLine("warn", "player_skin_probe_retained_member", std::format(
            "A->B->A confirmed but retained member selection was {} (caller RVAs remain available)",
            selected.status == Layout::RetainedOffsetStatus::Ambiguous ? "ambiguous" : "missing"));
        return;
    }
    void** vtable{};
    std::uint32_t imageSize{}, timestamp{};
    const auto moduleBase = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    if (!ReadField(player, 0, vtable) || !vtable || !ReadModuleIdentity(imageSize, timestamp)
        || reinterpret_cast<std::uintptr_t>(vtable) < moduleBase
        || reinterpret_cast<std::uintptr_t>(vtable) >= moduleBase + imageSize) {
        Diagnostics::LogLine("warn", "player_skin_probe_retained_member",
            "retained member was found but the LocalPlayer vtable failed module validation");
        return;
    }
    const auto updateRva = updateSkinTarget
        ? reinterpret_cast<std::uintptr_t>(updateSkinTarget) - moduleBase : 0;
    if (updateRva == 0) {
        Diagnostics::LogLine("warn", "player_skin_probe_retained_member",
            "retained member was found without a verified updateSkin target; descriptor was not persisted");
        return;
    }
    OwnershipDescriptor descriptor{OwnershipMagic, OwnershipVersion, imageSize, timestamp,
        static_cast<std::uint32_t>(selected.kind), updateRva,
        reinterpret_cast<std::uintptr_t>(vtable) - moduleBase, selected.byteOffset};
    SaveOwnershipDescriptor(descriptor);
    Diagnostics::LogLine("info", "player_skin_probe_retained_member", std::format(
        "A->B->A validated: selected Player+{:#x} kind={} vtableRva={:#x}; descriptor persisted locally",
        selected.byteOffset, descriptor.pointerKind, descriptor.localPlayerVtableRva));
}

// ---------------------------------------------------------------------------
// Delivery
// ---------------------------------------------------------------------------

// Copies the atlas into memory we own before it reaches the overlay. The game
// can free or re-upload its Blob at any moment, so the renderer must never
// hold a pointer into the game's heap.
bool DeliverCandidate(const Layout::SkinCandidate& candidate, std::string_view event,
    std::string_view origin) noexcept {
    const auto byteCount = static_cast<std::size_t>(
        Layout::ExpectedByteCount(candidate.width, candidate.height));
    const auto* pixels = reinterpret_cast<const std::uint8_t*>(candidate.pixelsAddress);

    std::vector<std::uint8_t> owned;
    try {
        owned.resize(byteCount);
    } catch (...) {
        StageLog("warn", event, std::format("could not allocate {} bytes for the skin copy", byteCount));
        return false;
    }
    if (!IsReadable(pixels, byteCount) || !CopyGuarded(owned.data(), pixels, byteCount)) {
        StageLog("warn", event, std::format(
            "skin pixels vanished mid-copy: origin={} skin={:#x} pixels={:#x} bytes={}",
            origin, candidate.skinAddress, candidate.pixelsAddress, byteCount));
        return false;
    }
    if (!Layout::HasUsableHeadPixels(owned, candidate.width, candidate.height)) {
        StageLog("warn", event, std::format(
            "rejected skin-shaped image without a usable base-layer face: origin={} skin={:#x}",
            origin, candidate.skinAddress));
        return false;
    }

    const auto revision = Layout::HashPixels(owned);
    if (revision == deliveredRevision.load(std::memory_order_relaxed)) {
        StageLog("info", event, std::format(
            "skin revision already active: origin={} revision={:#x}", origin, revision));
        return true;
    }
    if (!Dx12Overlay::UpdatePlayerSkin(candidate.width, candidate.height, owned, revision)) {
        StageLog("warn", event, std::format(
            "overlay rejected the skin upload: width={} height={}", candidate.width, candidate.height));
        return false;
    }
    deliveredRevision.store(revision, std::memory_order_relaxed);
    SaveSkinCache(candidate.width, candidate.height, owned, revision);
    Diagnostics::LogLine("info", event, std::format(
        "equipped skin delivered: origin={} skin={:#x} width={} height={} format={} persona={} primaryUser={} revision={:#x}",
        origin, candidate.skinAddress, candidate.width, candidate.height,
        candidate.imageFormat, candidate.persona, candidate.primaryUser, revision));
    return true;
}

// Picks the best candidate and refuses to deliver anything that is not the
// local account's own skin - rendering a stranger's head would be worse than
// keeping the embedded fallback.
bool ChooseAndDeliver(std::span<const Layout::SkinCandidate> candidates, std::string_view event,
    std::string_view origin, bool allowSingleUnmarked = false) noexcept {
    const auto chosen = Layout::ChooseSkinCandidate(candidates);
    if (chosen == Layout::NoCandidate) {
        StageLog("warn", event, std::format("origin={} no candidate had a supported skin extent", origin));
        return false;
    }
    const bool soleAuthoritativeCandidate = allowSingleUnmarked && candidates.size() == 1
        && candidates[chosen].fromPlayerMap;
    if (!candidates[chosen].primaryUser && !soleAuthoritativeCandidate) {
        StageLog("warn", event, std::format(
            "origin={} candidates={} but none had PlayerSkin::mIsPrimaryUser set; "
            "keeping the embedded fallback rather than rendering another player's skin",
            origin, candidates.size()));
        return false;
    }
    if (!candidates[chosen].primaryUser) {
        StageLog("info", event,
            "the player map contained one usable skin without mIsPrimaryUser; accepting it as the local skin");
    }
    return DeliverCandidate(candidates[chosen], event, origin);
}

// ---------------------------------------------------------------------------
// Event-driven path: Player::updateSkin(SerializedSkinRef const&, int)
// ---------------------------------------------------------------------------

struct UpdateSkinCallbackScope {
    UpdateSkinCallbackScope() noexcept { activeUpdateSkinCallbacks.fetch_add(1, std::memory_order_acq_rel); }
    ~UpdateSkinCallbackScope() { activeUpdateSkinCallbacks.fetch_sub(1, std::memory_order_acq_rel); }
};

void ObserveSerializedSkinRef(void* player, const void* skinReference, int clientSubId,
    const HookProbeContext& probe) noexcept {
    StageLog("info", "player_skin_update_invoked",
        std::format("Player::updateSkin callback observed (clientSubId={} reference={:#x})",
            clientSubId, reinterpret_cast<std::uintptr_t>(skinReference)));
    if (!skinReference) {
        StageLog("warn", "player_skin_update_event", "Player::updateSkin supplied a null SerializedSkinRef");
        return;
    }

    const void* owner{};
    if (!ReadField(skinReference, Layout::SharedPtrObjectOffset, owner) || !owner) {
        StageLog("warn", "player_skin_update_event",
            std::format("SerializedSkinRef had no ThreadOwner object (clientSubId={})", clientSubId));
        return;
    }

    const auto skinAddress = Layout::ThreadOwnedSkinAddress(reinterpret_cast<std::uint64_t>(owner));
    const void* skin = reinterpret_cast<const void*>(skinAddress);
    Layout::SkinCandidate candidate{};
    std::string_view reason = "none";
    if (!BuildCandidate(skin, false, candidate, reason)) {
        StageLog("warn", "player_skin_update_event", std::format(
            "SerializedSkinRef rejected: reason={} clientSubId={} skin={:#x}",
            reason, clientSubId, reinterpret_cast<std::uintptr_t>(skin)));
        return;
    }
    const auto revision = CandidateRevision(candidate);
    RecordProbeObservation(player, skinReference, owner, candidate, revision, probe);
    if (!probe.localPlayer && !Layout::IsPrimaryUpdateCandidate(candidate)) {
        StageLog("info", "player_skin_update_event", std::format(
            "ignored non-local Player::updateSkin callback (clientSubId={} type={} skin={:#x} format={} atlas={}x{})",
            clientSubId, probe.typeName, candidate.skinAddress, candidate.imageFormat, candidate.width, candidate.height));
        return;
    }
    Diagnostics::LogLine("info", "player_skin_update_ownership", std::format(
        "accepted update: clientSubId={} type={} rttiLocal={} primaryFlag={} revision={:#x}", clientSubId,
        probe.typeName, probe.localPlayer, candidate.primaryUser, revision));
    if (DeliverCandidate(candidate, "player_skin_update_event", "player_update_skin")) {
        authoritativeDelivered.store(true, std::memory_order_release);
    }
}

void HookedUpdateSkin(void* player, const void* skinReference, int clientSubId) noexcept {
    UpdateSkinCallbackScope callback;
    const auto probe = BeginHookProbe(player);
    if (originalUpdateSkin) originalUpdateSkin(player, skinReference, clientSubId);
    if (skinHookStopping.load(std::memory_order_acquire)) return;
    ObserveSerializedSkinRef(player, skinReference, clientSubId, probe);
}

bool InstallUpdateSkinHook() noexcept {
    // Do not restore the tempting E0 variant below. Static disassembly of the
    // 1.26.5203 image proved that its unique match is a constructor: RCX is the
    // object being initialized, RDX is copied to RDI, and fields at RCX+0x10
    // onward are cleared. It is not Player::updateSkin and detouring it caused
    // the dressing-room crash. Keep it here only as a diagnostic guard so a
    // future edit cannot silently re-introduce the false positive.
    constexpr std::string_view rejectedConstructorSignature =
        "48 89 5C 24 ? 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 ? ? ? ? "
        "48 81 EC ? ? ? ? 48 8B 05 ? ? ? ? 48 33 C4 48 89 85 ? ? ? ? 45 8B E0";
    constexpr std::string_view legacySignature =
        "48 89 5C 24 ? 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 ? ? ? ? "
        "48 81 EC ? ? ? ? 48 8B 05 ? ? ? ? 48 33 C4 48 89 85 ? ? ? ? 45 8B F0";
    const auto section = hat::process::get_process_module().get_section_data(".text");
    if (section.empty()) return false;
    auto* const begin = section.data();
    auto* const end = begin + section.size();

    struct SignatureMatches {
        std::array<std::uintptr_t, 2> addresses{};
        std::size_t count{};
    };
    const auto findMatches = [begin, end](std::string_view signature) {
        SignatureMatches matches{};
        const auto parsed = hat::parse_signature(signature);
        if (!parsed.has_value()) return matches;
        auto* cursor = begin;
        while (cursor < end && matches.count < matches.addresses.size()) {
            const auto found = hat::find_pattern(cursor, end, parsed.value());
            if (!found.has_result()) break;
            matches.addresses[matches.count++] = reinterpret_cast<std::uintptr_t>(found.get());
            cursor = found.get() + 1;
        }
        return matches;
    };

    const auto rejectedMatches = findMatches(rejectedConstructorSignature);
    const auto legacyMatches = findMatches(legacySignature);
    if (rejectedMatches.count != 0) {
        const auto moduleBase = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        Diagnostics::LogLine("warn", "player_skin_update_signature", std::format(
            "rejected {} known constructor match(es); firstRva={:#x}; this is not Player::updateSkin",
            rejectedMatches.count, rejectedMatches.addresses.front() - moduleBase));
    }
    const std::span<const std::uintptr_t> noCurrentMatches{};
    const auto selection = Layout::SelectUpdateSkinSignature(noCurrentMatches,
        std::span<const std::uintptr_t>(legacyMatches.addresses.data(), legacyMatches.count));
    if (selection.variant == Layout::UpdateSkinSignatureVariant::Missing) {
        Diagnostics::LogFuzzySignatureScan("player_skin_update_signature_fuzzy", legacySignature, 2, 8);
        Diagnostics::LogLine("warn", "player_skin_update_signature",
            "verified Player::updateSkin signature was not found; bounded heap fallback remains active");
        return false;
    }
    if (selection.variant == Layout::UpdateSkinSignatureVariant::Ambiguous) {
        Diagnostics::LogLine("warn", "player_skin_update_signature", std::format(
            "Player::updateSkin legacy signature was ambiguous (matches={}); refusing to hook",
            legacyMatches.count));
        return false;
    }

    constexpr std::string_view selectedName = "legacy_f0";
    updateSkinTarget = reinterpret_cast<void*>(selection.address);
    skinHookStopping.store(false, std::memory_order_release);
    if (!HookManager::Create(updateSkinTarget, reinterpret_cast<void*>(&HookedUpdateSkin),
            reinterpret_cast<void**>(&originalUpdateSkin))) {
        updateSkinTarget = nullptr;
        originalUpdateSkin = nullptr;
        Diagnostics::LogLine("warn", "player_skin_update_hook", "Player::updateSkin hook installation failed");
        return false;
    }
    const auto moduleBase = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    Diagnostics::LogLine("info", "player_skin_update_hook", std::format(
        "Player::updateSkin hook installed: variant={} address={:#x} rva={:#x}", selectedName,
        reinterpret_cast<std::uintptr_t>(updateSkinTarget), selection.address - moduleBase));
    return true;
}

// ---------------------------------------------------------------------------
// Authoritative path: ClientInstance -> LocalPlayer -> Level -> player map
// ---------------------------------------------------------------------------

struct PlayerMapView {
    const std::uint8_t* sentinel{};
    std::uint64_t size{};
};

// Reads the scalar fields of a candidate std::unordered_map and rejects
// anything that is not shaped like one. See PlayerSkinLayout.hpp for why the
// sentinel lives at +0x08 and not at the map base.
bool ReadPlayerMap(const void* map, PlayerMapView& view) noexcept {
    if (!IsReadable(map, Layout::MapSize)) return false;
    std::uint64_t sentinel{}, size{}, bucketFirst{}, bucketLast{}, bucketEnd{}, mask{};
    if (!ReadField(map, Layout::MapListHeadOffset, sentinel)) return false;
    if (!ReadField(map, Layout::MapListSizeOffset, size)) return false;
    if (!ReadField(map, Layout::MapBucketFirstOffset, bucketFirst)) return false;
    if (!ReadField(map, Layout::MapBucketLastOffset, bucketLast)) return false;
    if (!ReadField(map, Layout::MapBucketEndOffset, bucketEnd)) return false;
    if (!ReadField(map, Layout::MapMaskOffset, mask)) return false;
    if (!Layout::PlausiblePlayerMapShape(size, sentinel, bucketFirst, bucketLast, bucketEnd, mask)) return false;
    view.sentinel = reinterpret_cast<const std::uint8_t*>(sentinel);
    view.size = size;
    return true;
}

// The decisive check that an offset really is the player map: MSVC's list is
// circular through the sentinel, so walking `size` links must arrive back at
// the sentinel exactly, with every node's prev/next agreeing and every entry
// holding well-formed name/XUID strings. A wrong offset fails this almost
// immediately.
bool ValidatePlayerMapNodes(const PlayerMapView& view) noexcept {
    const std::uint8_t* node{};
    if (!ReadField(view.sentinel, Layout::NodeNextOffset, node)) return false;

    const std::uint8_t* previous = view.sentinel;
    for (std::uint64_t visited = 0; visited < view.size; ++visited) {
        if (node == nullptr || node == view.sentinel) return false;
        if (!IsReadable(node, Layout::NodeEntryOffset + Layout::EntrySkinHandleOffset + sizeof(void*))) return false;

        const std::uint8_t* back{};
        if (!ReadField(node, Layout::NodePrevOffset, back)) return false;
        if (back != previous) return false;

        const auto* entry = node + Layout::NodeEntryOffset;
        for (const auto stringOffset : {Layout::EntryNameOffset, Layout::EntryXuidOffset,
                 Layout::EntryPlatformOnlineIdOffset}) {
            std::uint64_t size{};
            std::uint64_t capacity{};
            if (!ReadField(entry, stringOffset + Layout::StringSizeOffset, size)) return false;
            if (!ReadField(entry, stringOffset + Layout::StringCapacityOffset, capacity)) return false;
            if (!Layout::PlausibleStringFields(size, capacity)) return false;
        }

        previous = node;
        if (!ReadField(node, Layout::NodeNextOffset, node)) return false;
    }
    // The walk must land exactly on the sentinel - one node short or long means
    // the declared size and the real list disagree, so this is not our map.
    return node == view.sentinel;
}

// Resolves Level::getPlayerMap by validation rather than assumption. The
// Flarial-maintained value for 1.26.X (0x4E0) is tried first; the remaining
// documented candidates only get a look if it fails structurally.
bool ResolvePlayerMap(const void* level, PlayerMapView& view, std::ptrdiff_t& offsetUsed) noexcept {
    const auto tryOffset = [&](std::ptrdiff_t offset) noexcept {
        const void* map{};
        if (!ReadField(level, static_cast<std::size_t>(offset), map)) return false;
        PlayerMapView candidate{};
        if (!ReadPlayerMap(map, candidate)) return false;
        if (!ValidatePlayerMapNodes(candidate)) return false;
        view = candidate;
        offsetUsed = offset;
        return true;
    };

    const auto cached = resolvedPlayerMapOffset.load(std::memory_order_relaxed);
    if (cached >= 0 && tryOffset(cached)) return true;

    for (const auto offset : Layout::PlayerMapOffsetCandidates) {
        if (offset == cached) continue;
        if (!tryOffset(offset)) continue;
        resolvedPlayerMapOffset.store(offset, std::memory_order_relaxed);
        Diagnostics::LogLine("info", "player_skin_map_offset", std::format(
            "Level::getPlayerMap validated at Level+{:#x} (entries={})", offset, view.size));
        return true;
    }
    if (cached >= 0) resolvedPlayerMapOffset.store(-1, std::memory_order_relaxed);
    return false;
}

void ObservePlayerMap(const PlayerMapView& view, std::ptrdiff_t mapOffset) noexcept {
    std::array<Layout::SkinCandidate, MaxCandidates> candidates{};
    std::size_t count = 0;
    std::size_t entriesSeen = 0;
    std::size_t handlesMissing = 0;
    std::string_view lastReason = "none";

    const std::uint8_t* node{};
    if (!ReadField(view.sentinel, Layout::NodeNextOffset, node)) return;

    for (std::uint64_t visited = 0; visited < view.size && node && node != view.sentinel; ++visited) {
        const std::uint8_t* nextNode{};
        if (!ReadField(node, Layout::NodeNextOffset, nextNode)) break;
        ++entriesSeen;

        const auto* entry = node + Layout::NodeEntryOffset;
        // PlayerSkin_1_21_90 is shared_ptr<ThreadOwner<PlayerSkin>>: slot 0 of
        // the shared_ptr is the object, and ThreadOwner declares mObject first.
        const void* owner{};
        if (ReadField(entry, Layout::EntrySkinHandleOffset + Layout::SharedPtrObjectOffset, owner) && owner) {
            const void* skin = static_cast<const std::uint8_t*>(owner) + Layout::ThreadOwnerObjectOffset;
            Layout::SkinCandidate candidate{};
            std::string_view reason = "none";
            if (BuildCandidate(skin, true, candidate, reason)) {
                if (count < candidates.size()) candidates[count++] = candidate;
            } else {
                lastReason = reason;
            }
        } else {
            ++handlesMissing;
        }
        node = nextNode;
    }

    if (count == 0) {
        StageLog("warn", "player_skin_entry_scan", std::format(
            "player map at Level+{:#x} had {} entries, {} with no skin handle, "
            "and no usable PlayerSkin (last reason={})",
            mapOffset, entriesSeen, handlesMissing, lastReason));
        return;
    }
    if (ChooseAndDeliver(std::span<const Layout::SkinCandidate>(candidates.data(), count),
            "player_skin_updated", "player_map", view.size == 1)) {
        authoritativeDelivered.store(true, std::memory_order_relaxed);
    }
}

// ---------------------------------------------------------------------------
// Secondary path: conservative heap search for the local player's PlayerSkin.
//
// This exists only because the authoritative path needs a ClientInstance, and
// the signature that yields one has gone stale before. It is deliberately
// anchored on the *whole* PlayerSkin object - five self-consistent strings, a
// well-formed RGBA atlas, and a flag block that must include
// mIsPrimaryUser - rather than on a bare mce::Image. The previous bare-image
// search matched GUI atlases, and its "top-left 8x8 must be transparent"
// heuristic rejected legitimate skins, so neither test is used any more.
// ---------------------------------------------------------------------------
// What the scan found, reported once per pass. The previous reader logged only
// "candidate images inspected=0", which proves a 64/128 RGBA atlas was never
// located but says nothing about *why* - so this census relaxes the test one
// condition at a time and reports each stage separately. It anchors on the
// width/height pair (the most distinctive part of an mce::Image, and the part
// that does not depend on the format enum being what we think it is) and then
// reports what the surrounding bytes actually look like.
struct ImageSample {
    std::int32_t format{};
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t depth{};
    std::uint8_t usage{};
    std::uint64_t byteCount{};
    std::uint64_t address{};
};

struct ScanCensus {
    std::uint64_t bytesScanned{};
    std::uint32_t regionsPrivate{};
    std::uint32_t regionsMapped{};
    std::uint32_t regionsImage{};
    std::uint32_t extent64{};
    std::uint32_t extent128{};
    std::uint32_t formatRgba{};
    std::uint32_t formatOther{};
    std::uint32_t blobNull{};
    std::uint32_t blobSizeMismatch{};
    std::uint32_t blobSizeExact{};
    std::uint32_t pixelsUnreadable{};
    std::uint32_t skinValidated{};
    std::uint32_t primaryUser{};
    std::array<ImageSample, 4> samples{};
    std::uint32_t sampleCount{};
};

// One skin-shaped atlas found in memory, with whatever we could learn about
// the PlayerSkin that may surround it.
struct ImageHit {
    std::uint64_t imageAddress{};
    std::uint64_t pixelsAddress{};
    std::uint64_t skinAddress{};
    std::uint32_t width{};
    std::uint32_t height{};
    std::int32_t format{};
    bool skinValidated{};
    bool primaryUser{};
    bool persona{};
};

void RecordImageHit(const ImageHit& hit) noexcept {
    try {
        const auto byteCount = static_cast<std::size_t>(
            Layout::ExpectedByteCount(hit.width, hit.height));
        std::vector<std::uint8_t> pixels(byteCount);
        if (!IsReadable(reinterpret_cast<const void*>(hit.pixelsAddress), byteCount)
            || !CopyGuarded(pixels.data(), reinterpret_cast<const void*>(hit.pixelsAddress), byteCount)) return;
        const auto revision = Layout::HashPixels(pixels);
        std::string skinId;
        if (hit.skinValidated) {
            skinId = ReadGameString(reinterpret_cast<const void*>(hit.skinAddress));
        }
        Diagnostics::LogLine("info", "player_skin_image_hit", std::format(
            "image={:#x} skin={:#x} pixels={:#x} atlas={}x{} format={} validated={} "
            "primary={} persona={} skinId={} revision={:#x}",
            hit.imageAddress, hit.skinAddress, hit.pixelsAddress, hit.width, hit.height, hit.format,
            hit.skinValidated, hit.primaryUser, hit.persona,
            skinId.empty() ? "<none>" : skinId, revision));
        if (!probeEnabled) return;
        const auto directory = AnalysisDirectory();
        std::filesystem::create_directories(directory);
        const auto path = directory / std::format("skin-hit-{:016x}-{}x{}.rgba", revision,
            hit.width, hit.height);
        if (std::filesystem::exists(path)) return;
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(pixels.data()),
            static_cast<std::streamsize>(pixels.size()));
    } catch (...) {}
}

constexpr std::size_t MaxImageHits = 40;

struct ScanState {
    ScanCensus census{};
    std::array<ImageHit, MaxImageHits> hits{};
    std::size_t hitCount{};
    bool hitsTruncated{};
    bool primaryCaptured{};
};

// POD-only and SEH-wrapped: a region can be freed between the VirtualQuery
// that cleared it and the reads here.
void ScanRegionForSkins(const std::uint8_t* begin, std::size_t size, ScanState* state) noexcept {
    if (size < Layout::ImageSize) return;
    const std::size_t limit = size - Layout::ImageSize;

    // Fused 8-byte probe over mce::Image's { width, height } pair. Anchoring on
    // the extent rather than the format means a wrong guess about ImageFormat
    // shows up in the census instead of silently skipping every atlas.
    const std::uint64_t extent64 = 64ULL | (64ULL << 32);
    const std::uint64_t extent128 = 128ULL | (128ULL << 32);

    __try {
        // CRT memchr is vectorized on supported Windows targets. Looking for
        // the low byte of 64/128 first avoids issuing an unaligned 64-bit read
        // for every eight bytes of hundreds of MiB of heap.
        const auto* cursor = begin + Layout::ImageWidthOffset;
        const auto* const lastWidth = begin + limit + Layout::ImageWidthOffset;
        while (cursor <= lastWidth) {
            if (scanStopping.load(std::memory_order_relaxed)
                || authoritativeDelivered.load(std::memory_order_acquire)) return;
            const auto remaining = static_cast<std::size_t>(lastWidth - cursor + 1);
            const auto* next64 = static_cast<const std::uint8_t*>(std::memchr(cursor, 64, remaining));
            const auto* next128 = static_cast<const std::uint8_t*>(std::memchr(cursor, 128, remaining));
            const std::uint8_t* widthAddress{};
            if (next64 && next128) widthAddress = (std::min)(next64, next128);
            else widthAddress = next64 ? next64 : next128;
            if (!widthAddress) break;
            cursor = widthAddress + 1;
            const std::uint8_t* image = widthAddress - Layout::ImageWidthOffset;
            const auto offset = static_cast<std::size_t>(image - begin);
            if ((offset & 7U) != 0) continue;
            std::uint64_t extent{};
            std::memcpy(&extent, image + Layout::ImageWidthOffset, sizeof(extent));
            if (extent != extent64 && extent != extent128) continue;

            const auto width = static_cast<std::uint32_t>(extent & 0xFFFFFFFFULL);
            const auto height = static_cast<std::uint32_t>(extent >> 32);
            if (width == 64) ++state->census.extent64; else ++state->census.extent128;

            std::int32_t format{};
            std::uint32_t depth{};
            std::uint64_t byteCount{};
            const std::uint8_t* pixels{};
            std::memcpy(&format, image + Layout::ImageFormatOffset, sizeof(format));
            std::memcpy(&depth, image + Layout::ImageDepthOffset, sizeof(depth));
            std::memcpy(&byteCount, image + Layout::ImageByteCountOffset, sizeof(byteCount));
            std::memcpy(&pixels, image + Layout::ImagePixelsOffset, sizeof(pixels));
            const std::uint8_t usage = image[Layout::ImageUsageOffset];

            if (state->census.sampleCount < state->census.samples.size()) {
                auto& sample = state->census.samples[state->census.sampleCount++];
                sample.format = format;
                sample.width = width;
                sample.height = height;
                sample.depth = depth;
                sample.usage = usage;
                sample.byteCount = byteCount;
                sample.address = reinterpret_cast<std::uint64_t>(image);
            }

            if (!Layout::IsRgbaSkinFormat(format)) { ++state->census.formatOther; continue; }
            ++state->census.formatRgba;

            if (pixels == nullptr) { ++state->census.blobNull; continue; }
            if (byteCount != Layout::ExpectedByteCount(width, height)) {
                ++state->census.blobSizeMismatch;
                continue;
            }
            ++state->census.blobSizeExact;
            if (!IsReadable(pixels, static_cast<std::size_t>(byteCount))) {
                ++state->census.pixelsUnreadable;
                continue;
            }

            ImageHit hit{};
            hit.imageAddress = reinterpret_cast<std::uint64_t>(image);
            hit.pixelsAddress = reinterpret_cast<std::uint64_t>(pixels);
            hit.width = width;
            hit.height = height;
            hit.format = format;

            // If this atlas is PlayerSkin::mSkinImage then the PlayerSkin base
            // is exactly 0xA0 earlier. Validating it is what yields
            // mIsPrimaryUser; an atlas that fails is still kept as a hit so the
            // uniqueness rule below can use it.
            if (offset >= Layout::SkinImageOffset
                && offset - Layout::SkinImageOffset + Layout::PlayerSkinSize <= size) {
                const std::uint8_t* skin = image - Layout::SkinImageOffset;
                if (SkinStringsPlausible(skin)) {
                    Layout::SkinCandidate candidate{};
                    std::string_view reason = "none";
                    if (BuildCandidate(skin, false, candidate, reason)) {
                        ++state->census.skinValidated;
                        if (candidate.primaryUser) ++state->census.primaryUser;
                        hit.skinValidated = true;
                        hit.primaryUser = candidate.primaryUser;
                        hit.persona = candidate.persona;
                        hit.skinAddress = candidate.skinAddress;
                    }
                }
            }
            if (state->hitCount < state->hits.size()) {
                state->hits[state->hitCount++] = hit;
            } else if (hit.primaryUser) {
                // Do not let unrelated 64x64 UI atlases found earlier in the
                // address space crowd the local player's validated skin out
                // of the fixed-size result buffer.
                const auto replace = std::find_if(state->hits.begin(), state->hits.end(),
                    [](const ImageHit& stored) { return !stored.primaryUser; });
                if (replace != state->hits.end()) *replace = hit;
                state->hitsTruncated = true;
            } else {
                state->hitsTruncated = true;
            }
            RecordImageHit(hit);
            if (hit.primaryUser) {
                state->primaryCaptured = true;
                return;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Fall through with whatever was collected before the fault.
    }
}

std::string FormatCensus(const ScanCensus& census, std::size_t hitCount, std::uint64_t elapsedMs) noexcept {
    try {
        std::string text = std::format(
            "regions(private={} mapped={} image={}) scanned={}MiB elapsed={}ms "
            "extent64={} extent128={} formatRgba={} formatOther={} "
            "blobNull={} blobSizeMismatch={} blobSizeExact={} pixelsUnreadable={} "
            "playerSkinValidated={} primaryUser={} hits={}",
            census.regionsPrivate, census.regionsMapped, census.regionsImage,
            census.bytesScanned / (1024 * 1024), elapsedMs,
            census.extent64, census.extent128, census.formatRgba, census.formatOther,
            census.blobNull, census.blobSizeMismatch, census.blobSizeExact,
            census.pixelsUnreadable, census.skinValidated, census.primaryUser, hitCount);
        for (std::uint32_t index = 0; index < census.sampleCount; ++index) {
            const auto& sample = census.samples[index];
            text += std::format(" | sample{}: {:#x} format={} {}x{} depth={} usage={} bytes={}",
                index, sample.address, sample.format, sample.width, sample.height,
                sample.depth, sample.usage, sample.byteCount);
        }
        return text;
    } catch (...) {
        return "census formatting failed";
    }
}

// Hashes a hit's pixels through an owned copy so distinct atlases can be told
// apart (Bedrock keeps the same skin alive at several addresses).
bool HashHit(const ImageHit& hit, std::uint64_t& hash) noexcept {
    const auto byteCount = static_cast<std::size_t>(Layout::ExpectedByteCount(hit.width, hit.height));
    std::vector<std::uint8_t> owned;
    try {
        owned.resize(byteCount);
    } catch (...) {
        return false;
    }
    const auto* pixels = reinterpret_cast<const std::uint8_t*>(hit.pixelsAddress);
    if (!IsReadable(pixels, byteCount) || !CopyGuarded(owned.data(), pixels, byteCount)) return false;
    if (!Layout::HasUsableHeadPixels(owned, hit.width, hit.height)) return false;
    hash = Layout::HashPixels(owned);
    return true;
}

bool EvaluateScanState(ScanState& state) noexcept {
    if (authoritativeDelivered.load(std::memory_order_acquire)) return false;
    if (state.hitCount == 0) return false;

    std::array<Layout::SkinCandidate, MaxImageHits> candidates{};
    std::size_t candidateCount = 0;
    bool anyPrimary = false;
    for (std::size_t index = 0; index < state.hitCount; ++index) {
        const auto& hit = state.hits[index];
        if (!hit.skinValidated) continue;
        Layout::SkinCandidate candidate{};
        candidate.skinAddress = hit.skinAddress;
        candidate.pixelsAddress = hit.pixelsAddress;
        candidate.width = hit.width;
        candidate.height = hit.height;
        candidate.primaryUser = hit.primaryUser;
        candidate.persona = hit.persona;
        candidate.fromPlayerMap = false;
        candidate.imageFormat = hit.format;
        candidates[candidateCount++] = candidate;
        anyPrimary = anyPrimary || hit.primaryUser;
    }
    if (anyPrimary) {
        StageLog("warn", "player_skin_heap_scan",
            "heap candidate claimed mIsPrimaryUser, but ownership offsets are unverified for this build; "
            "requiring unique usable pixels instead of trusting the stale flag");
    }

    std::array<std::uint64_t, MaxImageHits> hashes{};
    std::size_t distinct = 0;
    std::size_t representative = 0;
    for (std::size_t index = 0; index < state.hitCount; ++index) {
        std::uint64_t hash = 0;
        if (!HashHit(state.hits[index], hash)) continue;
        bool seen = false;
        for (std::size_t other = 0; other < distinct; ++other) {
            if (hashes[other] == hash) { seen = true; break; }
        }
        if (seen) continue;
        if (distinct == 0) representative = index;
        hashes[distinct++] = hash;
        if (distinct > 1) break;
    }

    if (distinct != 1 || state.hitsTruncated) {
        StageLog("warn", "player_skin_heap_scan", std::format(
            "{} stored skin-shaped atlases ({} distinct, truncated={}) but none had "
            "PlayerSkin::mIsPrimaryUser set; keeping the embedded fallback",
            state.hitCount, distinct, state.hitsTruncated));
        return false;
    }

    const auto& chosen = state.hits[representative];
    Layout::SkinCandidate unique{};
    unique.skinAddress = chosen.imageAddress;
    unique.pixelsAddress = chosen.pixelsAddress;
    unique.width = chosen.width;
    unique.height = chosen.height;
    unique.primaryUser = true;
    unique.persona = chosen.persona;
    unique.imageFormat = chosen.format;
    return DeliverCandidate(unique, "player_skin_heap_scan", "heap_scan_unique_atlas");
}

enum class ScanPassOutcome { InProgress, Found, CompleteNoMatch };

struct LocalPlayerScanProgress {
    std::uintptr_t address{};
    std::array<Layout::SkinCandidate, 4> candidates{};
    std::array<std::uint64_t, 4> revisions{};
    std::array<std::size_t, 4> memberOffsets{};
    std::size_t candidateCount{};
    std::size_t playerCount{};
};

bool ReadUsableCandidate(const void* skin, Layout::SkinCandidate& candidate,
    std::uint64_t& revision) noexcept {
    std::string_view reason;
    if (!BuildCandidate(skin, false, candidate, reason)) return false;
    const auto byteCount = static_cast<std::size_t>(
        Layout::ExpectedByteCount(candidate.width, candidate.height));
    std::vector<std::uint8_t> pixels;
    try { pixels.resize(byteCount); } catch (...) { return false; }
    if (!CopyGuarded(pixels.data(), reinterpret_cast<const void*>(candidate.pixelsAddress), byteCount)
        || !Layout::HasUsableHeadPixels(pixels, candidate.width, candidate.height)) return false;
    revision = Layout::HashPixels(pixels);
    return true;
}

void InspectLocalPlayer(void* player, LocalPlayerScanProgress& progress) noexcept {
    constexpr std::size_t MaxPlayerBytes = 0x5000;
    for (std::size_t offset = sizeof(void*); offset < MaxPlayerBytes; offset += sizeof(void*)) {
        void* member{};
        if (!ReadField(player, offset, member) || !member) continue;

        std::array<const void*, 3> possibleSkins{};
        possibleSkins[0] = member;
        void* owner{};
        if (ReadField(member, Layout::SharedPtrObjectOffset, owner) && owner) {
            possibleSkins[1] = reinterpret_cast<const void*>(
                Layout::ThreadOwnedSkinAddress(reinterpret_cast<std::uint64_t>(owner)));
        }
        possibleSkins[2] = reinterpret_cast<const void*>(
            Layout::ThreadOwnedSkinAddress(reinterpret_cast<std::uint64_t>(member)));

        for (const void* skin : possibleSkins) {
            if (!skin) continue;
            Layout::SkinCandidate candidate{};
            std::uint64_t revision{};
            if (!ReadUsableCandidate(skin, candidate, revision)) continue;
            bool duplicate{};
            for (std::size_t index = 0; index < progress.candidateCount; ++index) {
                if (progress.revisions[index] == revision) { duplicate = true; break; }
            }
            if (duplicate) continue;
            if (progress.candidateCount < progress.candidates.size()) {
                candidate.primaryUser = true;
                candidate.fromPlayerMap = true;
                const auto index = progress.candidateCount++;
                progress.candidates[index] = candidate;
                progress.revisions[index] = revision;
                progress.memberOffsets[index] = offset;
            }
        }
    }
}

// Late-injection ownership path that needs no version-specific field offset:
// find the one MSVC object whose RTTI is LocalPlayer-derived, then inspect its
// pointer members for a structurally valid SerializedSkinRef/PlayerSkin. This
// is slower than a learned descriptor, so it is chunked and time-bounded.
ScanPassOutcome ScanForLocalPlayerPass(LocalPlayerScanProgress& progress) noexcept {
    constexpr auto PassBudget = std::chrono::milliseconds(300);
    constexpr std::size_t ChunkBytes = 2 * 1024 * 1024;
    SYSTEM_INFO systemInfo{};
    GetSystemInfo(&systemInfo);
    const auto minimum = reinterpret_cast<std::uintptr_t>(systemInfo.lpMinimumApplicationAddress);
    const auto maximum = reinterpret_cast<std::uintptr_t>(systemInfo.lpMaximumApplicationAddress);
    if (progress.address == 0) progress.address = minimum;
    const auto started = Clock::now();
    std::uint32_t imageSize{}, timestamp{};
    if (!ReadModuleIdentity(imageSize, timestamp)) return ScanPassOutcome::CompleteNoMatch;
    (void)timestamp;
    const auto moduleBase = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    const auto moduleEnd = moduleBase + imageSize;

    while (progress.address < maximum && !scanStopping.load(std::memory_order_relaxed)
        && !authoritativeDelivered.load(std::memory_order_acquire)) {
        MEMORY_BASIC_INFORMATION information{};
        if (VirtualQuery(reinterpret_cast<void*>(progress.address), &information,
                sizeof(information)) != sizeof(information)) break;
        const auto regionBase = reinterpret_cast<std::uintptr_t>(information.BaseAddress);
        const auto regionEnd = regionBase + information.RegionSize;
        if (information.RegionSize == 0 || regionEnd <= progress.address) break;
        constexpr DWORD Writable = PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE;
        const bool scannable = information.State == MEM_COMMIT && information.Type == MEM_PRIVATE
            && (information.Protect & Writable) != 0
            && (information.Protect & (PAGE_GUARD | PAGE_NOACCESS)) == 0;
        if (!scannable) { progress.address = regionEnd; continue; }

        const auto chunkEnd = (std::min)(progress.address + ChunkBytes, regionEnd);
        auto cursor = (progress.address + sizeof(void*) - 1) & ~(sizeof(void*) - 1);
        for (; cursor + sizeof(void*) <= chunkEnd; cursor += sizeof(void*)) {
            std::uintptr_t vtable{};
            if (!CopyGuarded(&vtable, reinterpret_cast<const void*>(cursor), sizeof(vtable))
                || vtable < moduleBase || vtable >= moduleEnd) continue;
            const auto type = ResolveMsvcTypeName(reinterpret_cast<const void*>(cursor));
            if (!type || !Layout::IsLocalPlayerTypeName(*type)) continue;
            ++progress.playerCount;
            Diagnostics::LogLine("info", "player_skin_local_rtti", std::format(
                "LocalPlayer object found: player={:#x} type={} vtableRva={:#x}",
                cursor, *type, vtable - moduleBase));
            InspectLocalPlayer(reinterpret_cast<void*>(cursor), progress);
        }
        progress.address = chunkEnd;
        if (Clock::now() - started >= PassBudget) {
            StageLog("info", "player_skin_local_scan", std::format(
                "bounded RTTI pass: cursor={:#x} localPlayers={} usableSkins={}",
                progress.address, progress.playerCount, progress.candidateCount));
            return ScanPassOutcome::InProgress;
        }
    }

    if (progress.playerCount != 1 || progress.candidateCount != 1) {
        StageLog("warn", "player_skin_local_scan", std::format(
            "LocalPlayer ownership lookup rejected: localPlayers={} distinctUsableSkins={} "
            "(exactly one of each required)", progress.playerCount, progress.candidateCount));
        return ScanPassOutcome::CompleteNoMatch;
    }
    Diagnostics::LogLine("info", "player_skin_local_member", std::format(
        "validated retained skin at LocalPlayer+{:#x} revision={:#x}",
        progress.memberOffsets[0], progress.revisions[0]));
    return DeliverCandidate(progress.candidates[0], "player_skin_local_scan",
        "rtti_local_player_retained_skin") ? ScanPassOutcome::Found : ScanPassOutcome::CompleteNoMatch;
}

struct DescriptorScanProgress {
    std::uintptr_t address{};
    std::array<Layout::SkinCandidate, 2> candidates{};
    std::array<std::uintptr_t, 2> players{};
    std::size_t candidateCount{};
};

ScanPassOutcome ScanForDescriptorPlayerPass(DescriptorScanProgress& progress) noexcept {
    if (!ownershipDescriptor) return ScanPassOutcome::CompleteNoMatch;
    constexpr auto PassBudget = std::chrono::milliseconds(300);
    constexpr std::size_t ChunkBytes = 2 * 1024 * 1024;
    SYSTEM_INFO systemInfo{};
    GetSystemInfo(&systemInfo);
    const auto minimum = reinterpret_cast<std::uintptr_t>(systemInfo.lpMinimumApplicationAddress);
    const auto maximum = reinterpret_cast<std::uintptr_t>(systemInfo.lpMaximumApplicationAddress);
    if (progress.address == 0) progress.address = minimum;
    const auto started = Clock::now();
    const auto moduleBase = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    const auto expectedVtable = moduleBase + ownershipDescriptor->localPlayerVtableRva;

    while (progress.address < maximum && !scanStopping.load(std::memory_order_relaxed)) {
        MEMORY_BASIC_INFORMATION information{};
        if (VirtualQuery(reinterpret_cast<void*>(progress.address), &information, sizeof(information)) != sizeof(information)) break;
        const auto regionBase = reinterpret_cast<std::uintptr_t>(information.BaseAddress);
        const auto regionEnd = regionBase + information.RegionSize;
        if (information.RegionSize == 0 || regionEnd <= progress.address) break;
        constexpr DWORD Writable = PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE;
        const bool scannable = information.State == MEM_COMMIT && information.Type == MEM_PRIVATE
            && (information.Protect & Writable) != 0
            && (information.Protect & (PAGE_GUARD | PAGE_NOACCESS)) == 0;
        if (!scannable) {
            progress.address = regionEnd;
            continue;
        }
        const auto chunkEnd = (std::min)(progress.address + ChunkBytes, regionEnd);
        auto cursor = (progress.address + sizeof(void*) - 1) & ~(sizeof(void*) - 1);
        for (; cursor + sizeof(void*) <= chunkEnd; cursor += sizeof(void*)) {
            std::uintptr_t value{};
            if (!CopyGuarded(&value, reinterpret_cast<const void*>(cursor), sizeof(value))
                || value != expectedVtable) continue;
            const auto type = ResolveMsvcTypeName(reinterpret_cast<const void*>(cursor));
            if (!type || !Layout::IsLocalPlayerTypeName(*type)) continue;
            void* retained{};
            if (!ReadField(reinterpret_cast<const void*>(cursor),
                    static_cast<std::size_t>(ownershipDescriptor->retainedMemberOffset), retained) || !retained) continue;
            const void* skin = retained;
            Layout::SkinCandidate candidate{};
            std::string_view reason;
            if (!BuildCandidate(skin, false, candidate, reason)) continue;
            bool duplicate{};
            for (std::size_t index = 0; index < progress.candidateCount; ++index) {
                if (progress.players[index] == cursor) duplicate = true;
            }
            if (!duplicate && progress.candidateCount < progress.candidates.size()) {
                progress.players[progress.candidateCount] = cursor;
                progress.candidates[progress.candidateCount++] = candidate;
            }
        }
        progress.address = chunkEnd;
        if (Clock::now() - started >= PassBudget) {
            StageLog("info", "player_skin_descriptor_scan", std::format(
                "bounded LocalPlayer pass: cursor={:#x} validatedCandidates={}",
                progress.address, progress.candidateCount));
            return ScanPassOutcome::InProgress;
        }
    }

    if (progress.candidateCount != 1) {
        StageLog("warn", "player_skin_descriptor_scan", std::format(
            "late-injection descriptor lookup rejected: validatedCandidates={} (exactly one required)",
            progress.candidateCount));
        return ScanPassOutcome::CompleteNoMatch;
    }
    if (!DeliverCandidate(progress.candidates[0], "player_skin_descriptor_scan", "retained_local_player")) {
        return ScanPassOutcome::CompleteNoMatch;
    }
    authoritativeDelivered.store(true, std::memory_order_release);
    return ScanPassOutcome::Found;
}

struct ScanProgress {
    ScanState state{};
    std::uintptr_t address{};
    Clock::time_point cycleStarted{};
};

ScanPassOutcome ScanProcessForSkinPass(ScanProgress& progress) noexcept {
    constexpr auto PassBudget = std::chrono::milliseconds(300);
    constexpr std::size_t ChunkBytes = 2 * 1024 * 1024;
    SYSTEM_INFO systemInfo{};
    GetSystemInfo(&systemInfo);
    const auto minimum = reinterpret_cast<std::uintptr_t>(systemInfo.lpMinimumApplicationAddress);
    const auto maximum = reinterpret_cast<std::uintptr_t>(systemInfo.lpMaximumApplicationAddress);
    if (progress.address == 0) {
        progress.address = minimum;
        progress.cycleStarted = Clock::now();
    }
    const auto passStarted = Clock::now();

    while (progress.address < maximum
        && !scanStopping.load(std::memory_order_relaxed)
        && !authoritativeDelivered.load(std::memory_order_acquire)) {
        MEMORY_BASIC_INFORMATION information{};
        if (VirtualQuery(reinterpret_cast<void*>(progress.address), &information, sizeof(information)) != sizeof(information)) {
            break;
        }
        const auto regionBase = reinterpret_cast<std::uintptr_t>(information.BaseAddress);
        const auto regionEnd = regionBase + information.RegionSize;
        if (information.RegionSize == 0 || regionEnd <= progress.address) break;

        // PlayerSkin and its Blob are mutable C++ heap allocations. Restricting
        // the fallback search to writable private pages avoids walking the
        // game's multi-gigabyte executable/resource/GPU mappings byte by byte.
        // Besides making acquisition finish promptly, this lets Remove stop
        // the worker before an explicit DLL unload.
        constexpr DWORD Writable = PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE;
        const bool scannable = information.State == MEM_COMMIT
            && information.Type == MEM_PRIVATE
            && (information.Protect & Writable) != 0
            && (information.Protect & (PAGE_GUARD | PAGE_NOACCESS)) == 0;

        if (scannable) {
            if (progress.address == regionBase) ++progress.state.census.regionsPrivate;
            const auto chunkEnd = (std::min)(progress.address + ChunkBytes, regionEnd);
            const auto scanBegin = progress.address == regionBase
                ? progress.address : progress.address - Layout::SkinImageOffset;
            const auto scanSize = static_cast<std::size_t>(chunkEnd - scanBegin);
            progress.state.census.bytesScanned += chunkEnd - progress.address;
            ScanRegionForSkins(reinterpret_cast<const std::uint8_t*>(scanBegin), scanSize, &progress.state);
            progress.address = chunkEnd;
            if (authoritativeDelivered.load(std::memory_order_acquire)) return ScanPassOutcome::InProgress;
            if (progress.state.primaryCaptured) {
                return EvaluateScanState(progress.state) ? ScanPassOutcome::Found : ScanPassOutcome::CompleteNoMatch;
            }
        } else {
            progress.address = regionEnd;
        }

        if (Clock::now() - passStarted >= PassBudget) {
            StageLog("info", "player_skin_scan_progress", std::format(
                "bounded pass complete: cursor={:#x} scanned={}MiB extents={} hits={}",
                progress.address, progress.state.census.bytesScanned / (1024 * 1024),
                progress.state.census.extent64 + progress.state.census.extent128,
                progress.state.hitCount));
            return ScanPassOutcome::InProgress;
        }
    }

    const auto elapsedMs = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - progress.cycleStarted).count());
    StageLog("info", "player_skin_census",
        FormatCensus(progress.state.census, progress.state.hitCount, elapsedMs));
    return EvaluateScanState(progress.state) ? ScanPassOutcome::Found : ScanPassOutcome::CompleteNoMatch;
}

// Sleeps in small steps so Remove() is never kept waiting on a long sleep.
void SleepUnlessStopping(std::chrono::milliseconds duration) noexcept {
    const auto deadline = Clock::now() + duration;
    while (!scanStopping.load(std::memory_order_relaxed) && Clock::now() < deadline) Sleep(50);
}

// Runs one acquisition cycle per menu open and nothing in between. Each cycle
// is abandoned as soon as the menu closes so heap walking never overlaps
// gameplay.
DWORD WINAPI SkinScanWorker(void*) noexcept {
    DescriptorScanProgress descriptorProgress{};
    bool descriptorComplete = true;
    LocalPlayerScanProgress localProgress{};
    bool localComplete{};
    ScanProgress progress{};
    bool cycleActive{};
    while (!scanStopping.load(std::memory_order_relaxed)) {
        // Strictly secondary: once either event-driven or player-map telemetry
        // has produced a skin, the authoritative path owns refreshes.
        if (authoritativeDelivered.load(std::memory_order_relaxed)) {
            Diagnostics::LogLine("info", "player_skin_heap_scan",
                "authoritative skin telemetry is live; stopping the secondary heap scan");
            return 0;
        }
        if (refreshRequested.exchange(false, std::memory_order_acq_rel)) {
            // Give the cheap render-Actor read a chance to answer first; in a
            // world it normally does within a frame and ends this worker.
            SleepUnlessStopping(std::chrono::milliseconds(500));
            descriptorProgress = {};
            descriptorComplete = !ownershipDescriptor.has_value();
            localProgress = {};
            localComplete = false;
            progress = {};
            cycleActive = true;
            Diagnostics::LogLine("info", "player_skin_refresh",
                "menu open requested a LocalPlayer skin refresh");
            continue;
        }
        if (!cycleActive || !menuOpen.load(std::memory_order_acquire)) {
            cycleActive = false;
            Sleep(100);
            continue;
        }
        ScanPassOutcome outcome = ScanPassOutcome::CompleteNoMatch;
        bool ranDescriptor{};
        try {
            if (!descriptorComplete) {
                ranDescriptor = true;
                outcome = ScanForDescriptorPlayerPass(descriptorProgress);
                descriptorComplete = outcome != ScanPassOutcome::InProgress;
            } else if (!localComplete) {
                outcome = ScanForLocalPlayerPass(localProgress);
                localComplete = outcome != ScanPassOutcome::InProgress;
            } else {
                outcome = ScanProcessForSkinPass(progress);
            }
        } catch (...) {}
        if (outcome == ScanPassOutcome::InProgress) {
            Sleep(25);
            continue;
        }
        if (ranDescriptor && outcome == ScanPassOutcome::CompleteNoMatch) {
            // The versioned object lookup is exhausted; begin the generic
            // atlas fallback immediately rather than imposing its retry delay.
            continue;
        }
        // Cycle finished (found or exhausted). A dressing-room change is
        // picked up on the next menu open instead of by periodic re-polling.
        cycleActive = false;
        progress = {};
        localProgress = {};
        localComplete = false;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Embedded fallback
// ---------------------------------------------------------------------------
bool LoadEmbeddedFallback() noexcept {
    HMODULE module{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&resourceAnchor), &module)) return false;
    const HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(IDR_OINKSTER_SKIN), RT_RCDATA);
    if (!resource) return false;
    const HGLOBAL loaded = LoadResource(module, resource);
    auto* data = static_cast<std::uint8_t*>(LockResource(loaded));
    const DWORD dataSize = SizeofResource(module, resource);
    if (!data || dataSize == 0) return false;

    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninitialize = SUCCEEDED(initialized);
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;
    bool success = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&factory)))
        && SUCCEEDED(factory->CreateStream(&stream))
        && SUCCEEDED(stream->InitializeFromMemory(data, dataSize))
        && SUCCEEDED(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &decoder))
        && SUCCEEDED(decoder->GetFrame(0, &frame))
        && SUCCEEDED(factory->CreateFormatConverter(&converter))
        && SUCCEEDED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA,
            WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom));
    UINT width{}, height{};
    if (success) success = SUCCEEDED(converter->GetSize(&width, &height)) && width == 64 && height == 64;
    std::vector<std::uint8_t> pixels;
    if (success) {
        pixels.resize(static_cast<std::size_t>(width) * height * 4);
        success = SUCCEEDED(converter->CopyPixels(nullptr, width * 4,
            static_cast<UINT>(pixels.size()), pixels.data()));
    }
    if (success) success = Dx12Overlay::UpdatePlayerSkin(width, height, pixels, FallbackRevision);
    if (uninitialize) CoUninitialize();
    return success;
}

// ---------------------------------------------------------------------------
// ClientInstance::getLocalPlayer vtable index
// ---------------------------------------------------------------------------

// Resolves the vtable index by reading the displacement out of a call site, and
// refuses to produce one unless the signature matched *exactly once*. An
// ambiguous match would mean calling an arbitrary virtual function on the
// ClientInstance, which is not a risk worth taking for a cosmetic head.
bool ResolveLocalPlayerIndex(std::string_view signature, std::wstring& detail) noexcept {
    const auto parsed = hat::parse_signature(signature);
    if (!parsed.has_value()) {
        detail = L"the live-skin signature was invalid";
        return false;
    }
    const auto section = hat::process::get_process_module().get_section_data(".text");
    if (section.empty()) {
        detail = L"the host module exposed no .text section to scan";
        return false;
    }
    std::byte* const sectionBegin = section.data();
    std::byte* const sectionEnd = sectionBegin + section.size();
    const auto first = hat::find_pattern(sectionBegin, sectionEnd, parsed.value());
    if (!first.has_result()) {
        // Flarial's maintained 1.26.X pattern. A miss means this installed
        // build moved the bytes again; the fuzzy rescan writes the real
        // current bytes into the log so they can be read back instead of
        // guessed at.
        Diagnostics::LogFuzzySignatureScan("player_skin_signature_fuzzy", signature, 2, 8);
        detail = L"the live-skin reader did not match this build";
        return false;
    }
    const auto second = hat::find_pattern(first.get() + 1, sectionEnd, parsed.value());
    if (second.has_result()) {
        Diagnostics::LogLine("warn", "player_skin_signature_ambiguous", std::format(
            "the local-player signature matched more than once ({:#x} and {:#x}); refusing to guess a vtable index",
            reinterpret_cast<std::uintptr_t>(first.get()), reinterpret_cast<std::uintptr_t>(second.get())));
        detail = L"the live-skin signature was ambiguous in this build";
        return false;
    }

    const auto* bytes = reinterpret_cast<const std::uint8_t*>(first.get());
    std::int32_t vtableByteOffset{};
    if (!ReadField(bytes, 9, vtableByteOffset)) {
        detail = L"the live-skin reader address was unreadable";
        return false;
    }
    if (vtableByteOffset <= 0 || vtableByteOffset % static_cast<std::int32_t>(sizeof(void*)) != 0
        || vtableByteOffset > 4096) {
        detail = L"the live-skin vtable index failed validation";
        return false;
    }
    localPlayerVtableIndex = static_cast<std::size_t>(vtableByteOffset) / sizeof(void*);
    Diagnostics::LogLine("info", "player_skin_signature_matched", std::format(
        "ClientInstance::getLocalPlayer resolved at vtable index {} (site={:#x})",
        localPlayerVtableIndex, reinterpret_cast<std::uintptr_t>(first.get())));
    return true;
}

}

bool Install(std::wstring& detail) noexcept {
    Diagnostics::LogLine("info", "player_skin_build",
        "skin acquisition build v3 (cache-v2, verified-face, heap-primary-disabled)");
    // Reset acquisition state before the hook becomes callable. updateSkin can
    // run on another game thread immediately after MinHook enables it.
    scanStopping.store(false, std::memory_order_release);
    // Nothing reads game memory until the menu is first opened (RequestRefresh).
    refreshRequested.store(false, std::memory_order_release);
    menuOpen.store(false, std::memory_order_release);
    renderActorRefreshPending.store(false, std::memory_order_release);
    authoritativeDelivered.store(false, std::memory_order_release);
    deliveredRevision.store(FallbackRevision, std::memory_order_release);
    probeEnabled = ProbeRequested();
    {
        const std::lock_guard<std::mutex> guard(probeMutex);
        probeSequence.clear();
    }
    LoadOwnershipDescriptor();
    Diagnostics::LogLine("info", "player_skin_probe", probeEnabled
        ? "live ownership probe enabled; raw snapshots remain local under the analysis directory"
        : "live ownership probe disabled; create analysis/enable-skin-probe or set OINKSTER_SKIN_PROBE=1 to enable it");
    const bool cacheLoaded = LoadSkinCache();
    const bool fallbackLoaded = cacheLoaded || LoadEmbeddedFallback();
    Diagnostics::LogLine(fallbackLoaded ? "info" : "warn", "player_skin_fallback",
        cacheLoaded ? "cached equipped atlas used as startup image"
            : fallbackLoaded ? "embedded Oinkster atlas uploaded" : "embedded Oinkster atlas could not be decoded");

    skinHookStopping.store(false, std::memory_order_release);
    const bool updateHookInstalled = InstallUpdateSkinHook();
    if (!scanWorker) {
        scanWorker = CreateThread(nullptr, 0, SkinScanWorker, nullptr, 0, nullptr);
        if (!scanWorker) {
            Diagnostics::LogLine("warn", "player_skin_heap_scan", "secondary scan worker could not start");
        }
    }

    // Flarial dll-oss's maintained 1.26.X signature table (SigInit::init260,
    // "ClientInstance::getLocalPlayerIndex") ends this pattern in a real
    // instruction, "48 8B C8" (mov rcx, rax).
    constexpr std::string_view signature =
        "49 8B 00 49 8B C8 48 8B 80 ? ? ? ? FF 15 ? ? ? ? 48 85 C0 0F 84 ? ? ? ? 48 8B C8";
    std::wstring failure;
    if (!ResolveLocalPlayerIndex(signature, failure)) {
        if (updateHookInstalled) {
            detail = fallbackLoaded
                ? L"Embedded fallback loaded; event-driven equipped-skin reader ready."
                : L"Event-driven equipped-skin reader ready.";
            return true;
        }
        detail = fallbackLoaded
            ? L"Embedded Oinkster skin active; " + failure + L". The bounded heap reader is running."
            : L"Skin fallback unavailable and " + failure + L".";
        return fallbackLoaded;
    }

    attached.store(true);
    detail = fallbackLoaded
        ? L"Embedded fallback loaded; equipped-skin reader ready."
        : L"Equipped-skin reader ready.";
    return true;
}

void ObserveClientInstance(void* clientInstance) noexcept {
    if (!attached.load()) {
        StageLog("warn", "player_skin_unattached",
            "ObserveClientInstance called but Install() never resolved the local-player vtable index; "
            "the secondary heap reader is the only live acquisition path");
        return;
    }
    if (!clientInstance) return;
    const auto now = Clock::now();
    if (lastPoll.time_since_epoch().count() != 0 && now - lastPoll < std::chrono::seconds(1)) return;
    lastPoll = now;

    if (!IsReadable(clientInstance, sizeof(void*))) {
        StageLog("warn", "player_skin_client_instance", std::format(
            "clientInstance={:#x} is not readable", reinterpret_cast<std::uintptr_t>(clientInstance)));
        return;
    }
    void** vtable{};
    if (!ReadField(clientInstance, 0, vtable)
        || !IsReadable(vtable, (localPlayerVtableIndex + 1) * sizeof(void*))) {
        StageLog("warn", "player_skin_client_instance", "the localPlayer vtable slot is unreadable");
        return;
    }
    using GetLocalPlayerFn = void* (*)(void*);
    auto getLocalPlayer = reinterpret_cast<GetLocalPlayerFn>(vtable[localPlayerVtableIndex]);
    if (!getLocalPlayer) return;
    void* localPlayer = getLocalPlayer(clientInstance);
    if (!localPlayer) {
        // Entirely normal outside a world (main menu, loading screens).
        StageLog("info", "player_skin_local_player", "no LocalPlayer yet (not in a world)");
        return;
    }

    ObserveLocalPlayer(localPlayer);

    // ObserveLocalPlayer performs the direct retained-member acquisition.
    // Keep the player-map path below as an independent, authoritative
    // cross-check when the legacy ClientInstance source is available.

    void* level{};
    if (!ReadField(localPlayer, static_cast<std::size_t>(Layout::ActorLevelOffset), level) || !level) {
        StageLog("warn", "player_skin_level", std::format(
            "Actor::level at localPlayer({:#x})+{:#x} was unreadable or null",
            reinterpret_cast<std::uintptr_t>(localPlayer), Layout::ActorLevelOffset));
        return;
    }

    PlayerMapView view{};
    std::ptrdiff_t offsetUsed = 0;
    if (!ResolvePlayerMap(level, view, offsetUsed)) {
        StageLog("warn", "player_skin_map", std::format(
            "no structurally valid player map at level={:#x} for any known Level::getPlayerMap offset",
            reinterpret_cast<std::uintptr_t>(level)));
        return;
    }
    ObservePlayerMap(view, offsetUsed);
}

void ObserveLocalPlayer(void* localPlayer) noexcept {
    if (!localPlayer) return;
    // This runs on Minecraft's render thread every frame. The inspection below
    // walks ~20 KiB of the Actor and copies/hashes skin atlases, so it only
    // runs while a menu-open refresh is outstanding - never during gameplay.
    if (!menuOpen.load(std::memory_order_acquire)
        || !renderActorRefreshPending.load(std::memory_order_acquire)) return;

    const auto now = Clock::now();
    if (lastDirectPlayerPoll.time_since_epoch().count() != 0
        && now - lastDirectPlayerPoll < std::chrono::seconds(1)) return;
    lastDirectPlayerPoll = now;

    if (!IsReadable(localPlayer, sizeof(void*))) {
        StageLog("warn", "player_skin_render_actor",
            "_renderLevelPrep supplied an unreadable Actor pointer");
        return;
    }
    void** vtable{};
    std::uint32_t imageSize{}, timestamp{};
    const auto moduleBase = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    if (!ReadField(localPlayer, 0, vtable) || !vtable
        || !ReadModuleIdentity(imageSize, timestamp)
        || reinterpret_cast<std::uintptr_t>(vtable) < moduleBase
        || reinterpret_cast<std::uintptr_t>(vtable) >= moduleBase + imageSize) {
        StageLog("warn", "player_skin_render_actor",
            "_renderLevelPrep Actor rejected because its vtable was outside the Minecraft image");
        return;
    }
    (void)timestamp;

    // 1.26.5203 is clang-compiled and its Actor objects do not expose the
    // MSVC CompleteObjectLocator consumed by ResolveMsvcTypeName. Ownership
    // instead comes from the uniquely matched _renderLevelPrep contract: its
    // fourth argument is Actor& for the local render player. A module-backed
    // vtable plus exactly one fully validated retained atlas keeps this path
    // fail-closed without relying on unavailable compiler RTTI.
    const auto type = ResolveMsvcTypeName(localPlayer);

    LocalPlayerScanProgress progress{};
    progress.playerCount = 1;
    InspectLocalPlayer(localPlayer, progress);
    // One inspection per menu open. A failed attempt is not retried until the
    // next open, so an unexpected layout can't turn into a per-second hitch.
    renderActorRefreshPending.store(false, std::memory_order_release);
    if (progress.candidateCount != 1) {
        StageLog("warn", "player_skin_render_actor", std::format(
            "verified local render player had {} distinct usable retained skins; exactly one required",
            progress.candidateCount));
        return;
    }

    if (progress.revisions[0] != deliveredRevision.load(std::memory_order_relaxed)) {
        Diagnostics::LogLine("info", "player_skin_render_actor", std::format(
            "validated equipped skin at local render Actor+{:#x}: player={:#x} type={} revision={:#x}",
            progress.memberOffsets[0], reinterpret_cast<std::uintptr_t>(localPlayer),
            type.value_or("clang_rtti_unavailable"),
            progress.revisions[0]));
    }
    if (DeliverCandidate(progress.candidates[0], "player_skin_render_actor",
            "render_level_prep_local_player")) {
        authoritativeDelivered.store(true, std::memory_order_release);
    }
}

void RequestRefresh() noexcept {
    if (scanStopping.load(std::memory_order_acquire)) return;
    menuOpen.store(true, std::memory_order_release);
    renderActorRefreshPending.store(true, std::memory_order_release);
    refreshRequested.store(true, std::memory_order_release);
}

void NotifyMenuClosed() noexcept {
    menuOpen.store(false, std::memory_order_release);
    renderActorRefreshPending.store(false, std::memory_order_release);
}

void Remove() noexcept {
    skinHookStopping.store(true, std::memory_order_release);
    if (updateSkinTarget) {
        (void)HookManager::Disable(updateSkinTarget);
        // Bounded, like Dx12Overlay::Remove and MouseInputBlocker::Remove: a
        // wedged game thread must not turn eject into a permanent hang.
        for (unsigned attempt = 0;
             activeUpdateSkinCallbacks.load(std::memory_order_acquire) != 0 && attempt < 200; ++attempt) {
            Sleep(5);
        }
        if (activeUpdateSkinCallbacks.load(std::memory_order_acquire) != 0) {
            // Deliberately leak the trampoline rather than free memory a
            // live thread is still executing.
            Diagnostics::LogLine("warning", "player_skin_remove",
                "game thread still inside the updateSkin detour; leaving its trampoline installed");
            UnloadGuard::Block("player skin trampoline still had a live thread at detach");
        } else {
            (void)HookManager::Remove(updateSkinTarget);
            originalUpdateSkin = nullptr;
        }
        updateSkinTarget = nullptr;
    }
    scanStopping.store(true);
    if (scanWorker) {
        if (WaitForSingleObject(scanWorker, 5000) != WAIT_OBJECT_0) {
            Diagnostics::LogLine("warning", "player_skin_remove",
                "skin scan worker did not exit within 5s; abandoning its handle");
            // The worker is still executing code in this image.
            UnloadGuard::Block("skin scan worker was still running at detach");
        } else {
            CloseHandle(scanWorker);
        }
        scanWorker = nullptr;
    }
    attached.store(false);
    menuOpen.store(false);
    renderActorRefreshPending.store(false);
    refreshRequested.store(false);
    authoritativeDelivered.store(false);
    probeEnabled = false;
    ownershipDescriptor.reset();
    {
        const std::lock_guard<std::mutex> guard(probeMutex);
        probeSequence.clear();
    }
    localPlayerVtableIndex = 0;
    lastDirectPlayerPoll = {};
    activeUpdateSkinCallbacks.store(0, std::memory_order_release);
    resolvedPlayerMapOffset.store(-1);
    deliveredRevision = FallbackRevision;
    lastPoll = {};
    ResetStageLog();
}

}
