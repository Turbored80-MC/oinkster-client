#include "Oinkster/ResourcePackTelemetry.hpp"
#include "Oinkster/CrosshairTexture.hpp"
#include "Oinkster/Diagnostics.hpp"
#include "Oinkster/ModuleManager.hpp"
#include "Oinkster/SafeMemory.hpp"
#include "Oinkster/SupportedBuild.hpp"
#include "Oinkster/Modules/HudFormat.hpp"
#include "Oinkster/Modules/TelemetryLogic.hpp"
#include <Windows.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <format>
#include <string>
#include <vector>

namespace Oinkster::ResourcePackTelemetry {
namespace {

// Layouts on 1.26.5203:
//  - Actor+0x1D8 is the actor's ILevel* (see SessionTelemetry); ILevel vtable
//    slot +0x8C8 returns a ResourcePackManager& (the pack walk at RVA
//    0x1706A90 calls it through NonOwnerPointer<ILevel> and reads the stack).
//  - ResourcePackManager::getStack(type) is [this + 0x98 + type * 8] for the
//    four stacks (inlined with its assert at RVA 0x8E53C2): 0 level, 1 addon,
//    2 global.
//  - ResourcePackStack: std::vector<PackInstance> at +0x8; PackInstance is
//    0x128 bytes (the same walk adds 0x128).
//  - PackInstance+0x100 -> [ ] -> [+0x8] -> [ ] is the PackManifest (the pack
//    telemetry lambda at RVA 0x8E8590); PackManifest::getName (RVA 0x18F1010)
//    falls back to the raw std::string at +0x110; the pack type byte at +0x181
//    is 6 for resource packs. The header uuid is the two qwords at +0x150
//    and +0x158 (the same lambda formats them as "{:08x}-{:04x}-...").
constexpr std::size_t Actor_Level = 0x1D8;
constexpr std::size_t ILevel_ResourcePackManagerSlot = 0x8C8 / sizeof(void*);
constexpr std::size_t Manager_Stacks = 0x98;
constexpr std::size_t Stack_Instances = 0x8;
constexpr std::size_t PackInstanceSize = 0x128;
constexpr std::size_t PackInstance_Pack = 0x100;
constexpr std::size_t Manifest_Name = 0x110;
constexpr std::size_t Manifest_Type = 0x181;
constexpr std::size_t Manifest_Uuid = 0x150;
constexpr std::uint8_t ResourcePackType = 6;
constexpr std::size_t LevelStack = 0;
constexpr std::size_t GlobalStack = 2;
constexpr std::size_t MaxPacksPerStack = 64;
constexpr std::size_t MaxPacksShown = 32;
constexpr auto ReadInterval = std::chrono::seconds(3);

using GetterFn = void* (*)(void*);

std::atomic<ModuleManager*> moduleManager{};
// Render-thread only.
bool validated{};
SteadyTime lastRead{};
std::string lastSample;
std::uintptr_t textBegin{};
std::uintptr_t textEnd{};

bool ResolveText() noexcept {
    const auto* base = reinterpret_cast<const std::uint8_t*>(GetModuleHandleW(nullptr));
    IMAGE_DOS_HEADER dos{};
    IMAGE_NT_HEADERS64 nt{};
    if (!base || !SafeMemory::ReadField(base, 0, dos) || dos.e_magic != IMAGE_DOS_SIGNATURE
        || !SafeMemory::ReadField(base, static_cast<std::size_t>(dos.e_lfanew), nt) || nt.Signature != IMAGE_NT_SIGNATURE) {
        return false;
    }
    const std::size_t sections = static_cast<std::size_t>(dos.e_lfanew) + offsetof(IMAGE_NT_HEADERS64, OptionalHeader)
        + nt.FileHeader.SizeOfOptionalHeader;
    for (std::size_t index = 0; index < nt.FileHeader.NumberOfSections; ++index) {
        IMAGE_SECTION_HEADER section{};
        if (!SafeMemory::ReadField(base, sections + index * sizeof(section), section)) return false;
        if (std::memcmp(section.Name, ".text", 6) == 0) {
            textBegin = reinterpret_cast<std::uintptr_t>(base) + section.VirtualAddress;
            textEnd = textBegin + section.Misc.VirtualSize;
            return true;
        }
    }
    return false;
}

// POD-only so it needs no unwinding (a requirement for __try).
bool CallGetterGuarded(GetterFn getter, void* self, void*& result) noexcept {
    __try {
        result = getter(self);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

const void* ReadManager(const void* actor) noexcept {
    void* level{};
    if (!SafeMemory::ReadField(actor, Actor_Level, level) || !level) return nullptr;
    void* slot = SafeMemory::ReadVirtualSlot(level, ILevel_ResourcePackManagerSlot);
    const auto address = reinterpret_cast<std::uintptr_t>(slot);
    if (!slot || address < textBegin || address >= textEnd) return nullptr;
    void* manager{};
    if (!CallGetterGuarded(reinterpret_cast<GetterFn>(slot), level, manager)) return nullptr;
    return manager;
}

// Appends the resource packs of one stack (and their uuids, in the same
// order); false on any implausible read.
bool ReadStack(const void* manager, std::size_t type, std::vector<ResourcePackInfo>& packs, std::vector<std::string>& uuids) {
    const void* stack{};
    if (!SafeMemory::ReadField(manager, Manager_Stacks + type * sizeof(void*), stack)) return false;
    if (!stack) return true;  // an unused stack
    std::uintptr_t begin{};
    std::uintptr_t end{};
    if (!SafeMemory::ReadField(stack, Stack_Instances, begin) || !SafeMemory::ReadField(stack, Stack_Instances + sizeof(void*), end)) return false;
    if (end < begin || (end - begin) % PackInstanceSize != 0) return false;
    const std::size_t count = (end - begin) / PackInstanceSize;
    if (count > MaxPacksPerStack || (count != 0 && !begin)) return false;
    for (std::size_t index = 0; index < count; ++index) {
        const auto* instance = reinterpret_cast<const void*>(begin + index * PackInstanceSize);
        const void* link{};
        const void* pack{};
        const void* holder{};
        const void* manifest{};
        if (!SafeMemory::ReadField(instance, PackInstance_Pack, link) || !link
            || !SafeMemory::ReadField(link, 0, pack) || !pack
            || !SafeMemory::ReadField(pack, 0x8, holder) || !holder
            || !SafeMemory::ReadField(holder, 0, manifest) || !manifest) {
            return false;
        }
        std::uint8_t packType{};
        if (!SafeMemory::ReadField(manifest, Manifest_Type, packType)) return false;
        if (packType != ResourcePackType) continue;
        std::array<std::uint64_t, 2> uuid{};
        if (SafeMemory::ReadField(manifest, Manifest_Uuid, uuid) && (uuid[0] != 0 || uuid[1] != 0)
            && uuids.size() < MaxPacksPerStack * 2) {
            uuids.push_back(CrosshairTexture::FormatPackUuid(uuid[0], uuid[1]));
        }
        std::string name;
        if (!SafeMemory::ReadMsvcString(manifest, Manifest_Name, name, 128)) return false;
        auto display = Modules::CleanWorldName(name);
        if (display.empty() || packs.size() >= MaxPacksShown) continue;
        packs.push_back({std::move(display), {}});
    }
    return true;
}

}

void Attach(ModuleManager& modules) noexcept {
    validated = false;
    lastRead = {};
    lastSample.clear();
    const bool haveText = ResolveText();
    try {
        modules.SetCapability(Capability::ResourcePacks, haveText
            ? CapabilityStatus{CapabilityState::Pending, L"Waiting for the first validated resource-pack stack read in Minecraft "
                + std::wstring(SupportedBuild::DisplayVersion) + L"."}
            : CapabilityStatus{CapabilityState::Unavailable, L"Minecraft's code section could not be located."});
    } catch (...) {}
    if (!haveText) return;
    CrosshairTexture::Start();
    moduleManager.store(&modules, std::memory_order_release);
}

void Detach() noexcept {
    auto* modules = moduleManager.exchange(nullptr, std::memory_order_acq_rel);
    if (!modules) return;
    CrosshairTexture::Stop();
    try {
        modules->Publish(GameCrosshairEvent{SteadyClock::now(), nullptr});
        modules->SetCapability(Capability::ResourcePacks, {CapabilityState::Unavailable, L"World telemetry was detached."});
    } catch (...) {}
}

void Observe(const void* actor, SteadyTime now) noexcept {
    auto* modules = moduleManager.load(std::memory_order_acquire);
    if (!modules || !actor || now - lastRead < ReadInterval) return;
    lastRead = now;
    try {
        const void* manager = ReadManager(actor);
        if (!manager) return;
        ResourcePackState state;
        state.capturedAt = now;
        std::vector<std::string> uuids;
        if (!ReadStack(manager, LevelStack, state.activePacks, uuids) || !ReadStack(manager, GlobalStack, state.activePacks, uuids)) return;
        // The crosshair redrawn in third person comes from the same stacks.
        CrosshairTexture::Request(std::move(uuids));
        modules->Publish(GameCrosshairEvent{now, CrosshairTexture::Current()});
        if (!validated) {
            validated = true;
            modules->SetCapability(Capability::ResourcePacks, {CapabilityState::Available, {}});
            Diagnostics::LogLine("info", "resource_packs_validated", std::format("{} active", state.activePacks.size()));
        }
        std::string sample = std::to_string(state.activePacks.size());
        for (const auto& pack : state.activePacks) sample += " | " + Modules::ToUtf8(pack.name);
        if (sample != lastSample) {
            Diagnostics::LogLine("info", "resource_packs_sample", sample);
            lastSample = std::move(sample);
        }
        modules->Publish(state);
    } catch (...) {}
}

}
