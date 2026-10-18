#include "Oinkster/ReplayWorld.hpp"

#include "Oinkster/Diagnostics.hpp"
#include "Oinkster/HookManager.hpp"
#include "Oinkster/SafeMemory.hpp"
#include "Oinkster/ScreenTelemetry.hpp"
#include "Oinkster/SignatureScan.hpp"
#include "Oinkster/UnloadGuard.hpp"
#include <Windows.h>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <chrono>
#include <format>
#include <fstream>
#include <iterator>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <thread>

namespace Oinkster::ReplayWorld {
namespace {

// All three on 1.26.5203, found from LeviLamina's 26.51 headers plus the
// image (lambda type names give the owning functions; .pdata their bounds):
//
// MinecraftGame::_update (RVA 0x7D31E0), void(MinecraftGame* this). Every
//   lambda named <lambda_N>@?0??_update@MinecraftGame@@AEAAXXZ is built in
//   it. Runs once per frame on the main thread.
// LevelSettings::LevelSettings() (RVA 0x1744CC0), this only. Its stores
//   match LeviLamina's member list: seed +0, game type -1 at +0x08,
//   difficulty +0x10, force game type +0x14, generator 6 (Undefined) at
//   +0x18, Json::Value at +0x20, "minecraft:plains" spawn biome at +0x38,
//   chunk tick range 4 at +0xAC, limited world 16x16 at +0xB8; last store
//   at +0x538, so sizeof == 0x540.
// MinecraftGame::startLocalServerAsync (RVA 0x7FF020), the IGameServerStartup
//   override, slot 2 of the table at .rdata 0xE76EA70 (slots: deleting dtor,
//   canStartLocalServer "test byte [rcx+0x508],1", this, isHosting...
//   "cmp [rcx+0x15C0],0", _getServerInitTaskGroup "mov rax,[rcx+0x17A0]").
//   It starts with lea r15,[rcx-0x20], and that table's destructor adds -0x20:
//   the IGameServerStartup subobject is MinecraftGame+0x20. Arguments:
//   (this, Async<void>* sret, const std::string& levelId, const std::string&
//   levelName, const ContentIdentity&, const LevelSettings&, StartIntent).
//   The identity (16-byte UUID + valid byte), the settings (copy-constructed)
//   and levelId are all copied before anything is awaited.
constexpr std::string_view UpdateSignature =
    "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC 38 04 00 00 48 8D AC 24 80 00 00 00 0F 29 BD A0 03 00 00 "
    "66 0F 29 B5 90 03 00 00 48 C7 85 88 03 00 00 FE FF FF FF 48 89 CF 8B 05 ? ? ? ? 8B 0D ? ? ? ? "
    "65 48 8B 14 25 58 00 00 00 48 8B 0C CA";
constexpr std::string_view LevelSettingsCtorSignature =
    "55 56 57 48 81 EC C0 00 00 00 48 8D AC 24 80 00 00 00 0F 29 75 30 48 C7 45 28 FE FF FF FF 48 89 CE "
    "48 C7 01 00 00 00 00 C7 41 08 FF FF FF FF C6 41 0C 00 C7 41 10 04 00 00 00 C6 41 14 00 "
    "C7 41 18 06 00 00 00 48 83 C1 20";
constexpr std::string_view StartLocalServerSignature =
    "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC 18 06 00 00 48 8D AC 24 80 00 00 00 48 C7 85 90 05 00 00 "
    "FE FF FF FF 4C 89 CB 4D 89 C6 48 89 D6 48 89 CF 4C 8D 79 E0 4C 89 F9 E8 ? ? ? ? 84 C0 0F 85 ? ? ? ? "
    "B9 01 00 00 00 31 C0 F0 0F B1 8F 0C 05 00 00 0F 85 ? ? ? ? 49 8B 07 48 8B 80 10 02 00 00";

constexpr std::size_t GameServerStartupOffset = 0x20;
constexpr std::size_t CanStartSlot = 1;
constexpr std::size_t StartLocalServerSlot = 2;
constexpr std::size_t LevelSettingsSize = 0x540;

// LevelSettings fields (offsets confirmed by the constructor's own stores).
constexpr std::size_t GameTypeOffset = 0x08;
constexpr std::size_t DifficultyOffset = 0x10;
constexpr std::size_t ForceGameTypeOffset = 0x14;
constexpr std::size_t GeneratorOffset = 0x18;
constexpr std::size_t MultiplayerIntentOffset = 0x91;
constexpr std::size_t LanBroadcastOffset = 0x92;
constexpr std::size_t XblBroadcastOffset = 0x94;
constexpr std::size_t PlatformBroadcastOffset = 0x98;
constexpr std::int32_t Creative = 1;
constexpr std::int32_t Peaceful = 0;
constexpr std::int32_t VoidGenerator = 5;
constexpr std::int32_t NoMultiPlay = 0;

constexpr const char* LevelId = WorldFolderName;
constexpr const char* LevelName = WorldDisplayName;

using UpdateFn = void (*)(void* game);
using LevelSettingsCtorFn = void* (*)(void* settings);
using CanStartFn = bool (*)(const void* startup);
using StartLocalServerFn = void* (*)(void* startup, void* result, const void* levelId, const void* levelName,
    const void* contentIdentity, const void* settings, std::int32_t startIntent);

// The game's std::string (MSVC, release): 16-byte small buffer or heap
// pointer, size, capacity. Both names fit the small buffer.
struct GameString {
    char buffer[16];
    std::size_t size;
    std::size_t capacity;
};
static_assert(sizeof(WorldFolderName) <= 16 && sizeof(WorldDisplayName) <= 16);

GameString MakeGameString(std::string_view value) noexcept {
    GameString text{};
    std::memcpy(text.buffer, value.data(), value.size());
    text.size = value.size();
    text.capacity = 15;
    return text;
}

UpdateFn originalUpdate{};
void* updateTarget{};
LevelSettingsCtorFn levelSettingsCtor{};
std::uintptr_t startLocalServer{};
std::atomic_bool attached{};
std::atomic_bool requested{};
std::atomic_uint32_t activeCallbacks{};
std::mutex errorMutex;
std::string lastError;
// Whether the constructor's later stores sit where expected, so the
// broadcast fields can be set; checked once from its bytes.
bool broadcastFieldsVerified{};

struct CallbackGuard final {
    CallbackGuard() noexcept { activeCallbacks.fetch_add(1, std::memory_order_acq_rel); }
    ~CallbackGuard() { activeCallbacks.fetch_sub(1, std::memory_order_acq_rel); }
    CallbackGuard(const CallbackGuard&) = delete;
    CallbackGuard& operator=(const CallbackGuard&) = delete;
};

void SetError(std::string error) {
    Diagnostics::LogLine("warning", "replay_world", error);
    std::scoped_lock lock(errorMutex);
    lastError = std::move(error);
}

// Buffers outlive the call (they are static): nothing the game copies from
// them is freed underneath it.
alignas(16) std::array<std::byte, 0x800> settingsStorage{};
alignas(16) std::array<std::byte, 0x40> identityStorage{};
alignas(16) std::array<std::byte, 0x40> asyncStorage{};
GameString levelIdStorage{};
GameString levelNameStorage{};

// POD-only so __try needs no unwinding. 0 ok, 1 constructor faulted, 2 the
// start call faulted.
int CallGuarded(void* startup, StartLocalServerFn start) noexcept {
    __try {
        levelSettingsCtor(settingsStorage.data());
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 1;
    }
    auto* settings = reinterpret_cast<std::uint8_t*>(settingsStorage.data());
    const auto put32 = [settings](std::size_t offset, std::int32_t value) { std::memcpy(settings + offset, &value, 4); };
    put32(GameTypeOffset, Creative);
    put32(DifficultyOffset, Peaceful);
    settings[ForceGameTypeOffset] = 1;
    put32(GeneratorOffset, VoidGenerator);
    if (broadcastFieldsVerified) {
        settings[MultiplayerIntentOffset] = 0;
        settings[LanBroadcastOffset] = 0;
        put32(XblBroadcastOffset, NoMultiPlay);
        put32(PlatformBroadcastOffset, NoMultiPlay);
    }
    __try {
        (void)start(startup, asyncStorage.data(), &levelIdStorage, &levelNameStorage, identityStorage.data(),
            settingsStorage.data(), 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 2;
    }
    return 0;
}

void StartWorld(void* game) noexcept {
    try {
        if (ScreenTelemetry::Known() && !ScreenTelemetry::OnTitleScreen()) {
            SetError("A replay world can only be started from Minecraft's title screen.");
            return;
        }
        auto* startup = static_cast<std::uint8_t*>(game) + GameServerStartupOffset;
        void** vtable{};
        std::uintptr_t startSlot{};
        std::uintptr_t canStartSlot{};
        if (!SafeMemory::ReadField(startup, 0, vtable) || !vtable
            || !SafeMemory::ReadField(vtable, StartLocalServerSlot * sizeof(void*), startSlot)
            || !SafeMemory::ReadField(vtable, CanStartSlot * sizeof(void*), canStartSlot)) {
            SetError("The game's server-startup interface could not be read.");
            return;
        }
        // The one check everything rests on: this object's IGameServerStartup
        // table really holds the function the signature matched.
        if (startSlot != startLocalServer) {
            SetError(std::format("MinecraftGame+{:#x} is not the expected server-startup interface (slot {:#x}, want {:#x}).",
                GameServerStartupOffset, startSlot, startLocalServer));
            return;
        }
        if (!reinterpret_cast<CanStartFn>(canStartSlot)(startup)) {
            SetError("Minecraft is not ready to start a local world.");
            return;
        }
        levelIdStorage = MakeGameString(LevelId);
        levelNameStorage = MakeGameString(LevelName);
        identityStorage.fill(std::byte{});
        asyncStorage.fill(std::byte{});
        settingsStorage.fill(std::byte{});
        const int outcome = CallGuarded(startup, reinterpret_cast<StartLocalServerFn>(startSlot));
        if (outcome != 0) {
            SetError(outcome == 1 ? "Building the world settings faulted." : "Starting the replay world faulted.");
            return;
        }
        Diagnostics::LogLine("info", "replay_world", std::format("started local world id={} broadcastFields={}",
            LevelId, broadcastFieldsVerified));
        std::scoped_lock lock(errorMutex);
        lastError.clear();
    } catch (...) {
        SetError("Starting the replay world failed.");
    }
}

void HookedUpdate(void* game) {
    const CallbackGuard callback;
    if (const auto original = originalUpdate) original(game);
    if (!attached.load(std::memory_order_acquire) || !requested.exchange(false, std::memory_order_acq_rel)) return;
    StartWorld(game);
}

bool ContainsBytes(std::uintptr_t start, std::size_t length, std::string_view bytes) noexcept {
    std::array<std::uint8_t, 0x400> code{};
    if (length > code.size() || !SafeMemory::CopyGuarded(code.data(), reinterpret_cast<const void*>(start), length)) return false;
    const auto* needle = reinterpret_cast<const std::uint8_t*>(bytes.data());
    for (std::size_t at = 0; at + bytes.size() <= length; ++at) {
        if (std::memcmp(code.data() + at, needle, bytes.size()) == 0) return true;
    }
    return false;
}

}

bool Install(std::wstring& detail) noexcept {
    const auto find = [](std::string_view signature) {
        const std::array<std::string_view, 1> signatures{signature};
        return SignatureScan::FindUniqueInText(signatures);
    };
    const auto update = find(UpdateSignature);
    const auto ctor = find(LevelSettingsCtorSignature);
    const auto start = find(StartLocalServerSignature);
    Diagnostics::LogLine("info", "replay_world_signatures", std::format("update={} levelSettings={} startLocalServer={}",
        SignatureScan::OutcomeName(update.outcome), SignatureScan::OutcomeName(ctor.outcome),
        SignatureScan::OutcomeName(start.outcome)));
    if (update.outcome != SignatureScan::Outcome::Unique || ctor.outcome != SignatureScan::Outcome::Unique
        || start.outcome != SignatureScan::Outcome::Unique) {
        detail = L"A replay-world signature did not match uniquely; Watch falls back to opening a world by hand.";
        return false;
    }
    levelSettingsCtor = reinterpret_cast<LevelSettingsCtorFn>(ctor.address);
    startLocalServer = start.address;
    // mov byte [rsi+0x92],1 / mov [rsi+0x94],rax (the 3,3 broadcast pair).
    using namespace std::string_view_literals;
    broadcastFieldsVerified = ContainsBytes(ctor.address, 0x200, "\xC6\x86\x92\x00\x00\x00\x01"sv)
        && ContainsBytes(ctor.address, 0x200, "\x48\x89\x86\x94\x00\x00\x00"sv);
    void* target = reinterpret_cast<void*>(update.address);
    if (!HookManager::Create(target, reinterpret_cast<void*>(&HookedUpdate), reinterpret_cast<void**>(&originalUpdate))) {
        detail = L"The game-update hook could not be installed.";
        return false;
    }
    updateTarget = target;
    requested.store(false);
    attached.store(true, std::memory_order_release);
    detail = L"Replay world launcher attached (MinecraftGame::_update).";
    return true;
}

namespace {
std::jthread cleanup;
std::atomic_bool cleanupRunning{};

std::filesystem::path EnvironmentPath(const wchar_t* name) {
    std::array<wchar_t, MAX_PATH> buffer{};
    const DWORD length = GetEnvironmentVariableW(name, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) return {};
    return std::filesystem::path(std::wstring(buffer.data(), length));
}

// Minecraft's data roots: the GDK build's and the old UWP package's.
std::vector<std::filesystem::path> DataRoots() {
    std::vector<std::filesystem::path> roots;
    if (const auto roaming = EnvironmentPath(L"APPDATA"); !roaming.empty()) {
        roots.push_back(roaming / L"Minecraft Bedrock");
        roots.push_back(roaming / L"Minecraft Bedrock Preview");
    }
    if (const auto local = EnvironmentPath(L"LOCALAPPDATA"); !local.empty()) {
        roots.push_back(local / L"Packages" / L"Microsoft.MinecraftUWP_8wekyb3d8bbwe" / L"LocalState");
    }
    return roots;
}

void DeleteReplayWorlds(std::stop_token stop, bool (*abort)()) {
    constexpr int Attempts = 40;
    std::vector<std::filesystem::path> worlds;
    for (const auto& root : DataRoots()) {
        for (auto& world : FindReplayWorlds(root)) worlds.push_back(std::move(world));
    }
    for (const auto& world : worlds) {
        std::error_code error;
        bool removed = false;
        // The game can hold the world's database open for a moment after it
        // leaves; retry rather than leave a half-deleted world behind.
        for (int attempt = 0; attempt < Attempts && !stop.stop_requested() && !abort(); ++attempt) {
            error.clear();
            std::filesystem::remove_all(world, error);
            if (!error && !std::filesystem::exists(world, error)) {
                removed = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        Diagnostics::LogLine(removed ? "info" : "warning", "replay_world_delete", std::format("{} {}",
            removed ? "deleted" : "could not delete", world.string()) + (removed ? "" : std::format(" ({})", error.message())));
    }
}

}

std::size_t PrepareWorld(std::span<const ReplayBlockPack::PackFile> pack, std::string& error) {
    std::size_t written{};
    try {
        // Wherever the game keeps worlds: the shared folder and each user's.
        std::vector<std::filesystem::path> parents;
        std::error_code ignored;
        for (const auto& root : DataRoots()) {
            const auto shared = root / L"games" / L"com.mojang" / L"minecraftWorlds";
            if (std::filesystem::is_directory(shared, ignored)) parents.push_back(shared);
            for (std::filesystem::directory_iterator users(root / L"Users", ignored), end; !ignored && users != end; users.increment(ignored)) {
                const auto worlds = users->path() / L"games" / L"com.mojang" / L"minecraftWorlds";
                if (std::filesystem::is_directory(worlds, ignored)) parents.push_back(worlds);
            }
            ignored.clear();
        }
        for (const auto& parent : parents) {
            const auto world = parent / WorldFolderName;
            if (std::filesystem::exists(world, ignored)) {
                // Only ever a folder this client made (see FindReplayWorlds).
                std::ifstream nameFile(world / L"levelname.txt", std::ios::binary);
                std::string name((std::istreambuf_iterator<char>(nameFile)), std::istreambuf_iterator<char>());
                while (!name.empty() && (name.back() == '\n' || name.back() == '\r')) name.pop_back();
                if (name != WorldDisplayName) continue;
                nameFile.close();
                std::filesystem::remove_all(world);
            }
            if (pack.empty()) continue;
            const auto packFolder = world / L"behavior_packs" / std::filesystem::path(std::string(ReplayBlockPack::PackFolderName));
            const auto write = [](const std::filesystem::path& path, std::string_view text) {
                std::filesystem::create_directories(path.parent_path());
                std::ofstream file(path, std::ios::binary | std::ios::trunc);
                file.write(text.data(), static_cast<std::streamsize>(text.size()));
                if (!file) throw std::runtime_error("could not write " + path.string());
            };
            write(world / L"levelname.txt", WorldDisplayName);
            write(world / L"world_behavior_packs.json", ReplayBlockPack::WorldPacksJson());
            for (const auto& file : pack) write(packFolder / std::filesystem::path(file.path), file.text);
            ++written;
        }
        error.clear();
    } catch (const std::exception& exception) {
        error = exception.what();
        return 0;
    }
    return written;
}

std::vector<std::filesystem::path> FindReplayWorlds(const std::filesystem::path& root) {
    std::vector<std::filesystem::path> worlds;
    std::error_code error;
    const auto consider = [&](const std::filesystem::path& world) {
        std::error_code ignored;
        if (!std::filesystem::is_directory(world, ignored)) return;
        std::ifstream nameFile(world / L"levelname.txt", std::ios::binary);
        std::string name((std::istreambuf_iterator<char>(nameFile)), std::istreambuf_iterator<char>());
        while (!name.empty() && (name.back() == '\n' || name.back() == '\r')) name.pop_back();
        if (name == WorldDisplayName) worlds.push_back(world);
    };
    const auto worldsUnder = [](const std::filesystem::path& base) {
        return base / L"games" / L"com.mojang" / L"minecraftWorlds" / WorldFolderName;
    };
    consider(worldsUnder(root));
    for (std::filesystem::directory_iterator users(root / L"Users", error), end; !error && users != end; users.increment(error)) {
        if (users->is_directory(error)) consider(worldsUnder(users->path()));
    }
    return worlds;
}

void DeleteReplayWorldsAsync(bool (*abort)()) noexcept {
    if (!abort) return;
    if (cleanupRunning.exchange(true, std::memory_order_acq_rel)) return;
    try {
        if (cleanup.joinable()) cleanup.join();
        cleanup = std::jthread([abort](std::stop_token stop) {
            try {
                DeleteReplayWorlds(stop, abort);
            } catch (...) {}
            cleanupRunning.store(false, std::memory_order_release);
        });
    } catch (...) {
        cleanupRunning.store(false, std::memory_order_release);
    }
}

void Remove() noexcept {
    if (cleanup.joinable()) {
        cleanup.request_stop();
        cleanup.join();
    }
    attached.store(false, std::memory_order_release);
    requested.store(false);
    if (!updateTarget) return;
    (void)HookManager::Disable(updateTarget);
    for (unsigned attempt = 0; activeCallbacks.load(std::memory_order_acquire) != 0 && attempt < 200; ++attempt) Sleep(5);
    if (activeCallbacks.load(std::memory_order_acquire) != 0) {
        Diagnostics::LogLine("warning", "replay_world_remove", "a thread is still inside the game-update detour");
        UnloadGuard::Block("game-update trampoline still had a live thread at detach");
        return;
    }
    (void)HookManager::Remove(updateTarget);
    updateTarget = nullptr;
    originalUpdate = nullptr;
}

bool Available() noexcept { return attached.load(std::memory_order_acquire); }

void RequestStart() noexcept {
    if (!attached.load(std::memory_order_acquire)) return;
    {
        std::scoped_lock lock(errorMutex);
        lastError.clear();
    }
    requested.store(true, std::memory_order_release);
}

bool Pending() noexcept { return requested.load(std::memory_order_acquire); }

std::string LastError() {
    std::scoped_lock lock(errorMutex);
    return lastError;
}

}
