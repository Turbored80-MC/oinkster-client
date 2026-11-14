#include "Oinkster/CameraTelemetry.hpp"
#include "Oinkster/Diagnostics.hpp"
#include "Oinkster/ModuleManager.hpp"
#include "Oinkster/PresentationAdapter.hpp"
#include "Oinkster/SafeMemory.hpp"
#include <Windows.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <format>
#include <string>

namespace Oinkster::CameraTelemetry {
namespace {

// Layouts on 1.26.5203, derived from the build's own code:
//  - LocalPlayer+0xD70 is its ClientInstance: 21 call sites load it and call
//    vtable slot 0x578 (getOptions) before calling Options methods.
//  - ClientInstance::getOptions is "mov rax, [rcx+0xE10]; ret" (RVA
//    0x5DC28D0). The field offset is taken from those bytes at runtime, so
//    the game's own code decides what is read.
//  - Options keeps its options at +0x10 + id * 8 (Options::getOption, RVA
//    0xFB4B10; registration indexes by OptionImpl+0x1CC, ids < 0x336).
//  - Options::getPlayerViewPerspective (vtable slot 0x3C0, RVA 0xFB7430)
//    fetches option id 3, follows OptionImpl+0x238 (an override option)
//    until it is null, and returns the int at +0x18. Its bytes are checked
//    before any of this is trusted.
//  - The option registered for "game_thirdperson" has the telemetry name
//    "playerViewPerspective" at OptionImpl+0x1A8 (an MSVC std::string),
//    checked as well. IntOption keeps its value at +0x18.
constexpr std::size_t Actor_ClientInstance = 0xD70;
constexpr std::size_t ClientInstance_GetOptionsSlot = 0x578 / sizeof(void*);
constexpr std::size_t Options_PerspectiveSlot = 0x3C0 / sizeof(void*);
constexpr std::size_t Options_Table = 0x10;
constexpr std::uint32_t PerspectiveOptionId = 3;
constexpr std::size_t Option_Impl = 0x8;
constexpr std::size_t OptionImpl_TelemetryName = 0x1A8;
constexpr std::size_t OptionImpl_Id = 0x1CC;
constexpr std::size_t OptionImpl_Override = 0x238;
constexpr std::size_t IntOption_Value = 0x18;
constexpr std::size_t MaxOverrideHops = 8;
constexpr std::string_view PerspectiveTelemetryName = "playerViewPerspective";
constexpr auto RepublishInterval = std::chrono::milliseconds(100);

std::atomic<ModuleManager*> moduleManager{};
// Render-thread only.
std::uintptr_t textBegin{};
std::uintptr_t textEnd{};
const void* validatedOptions{};
const void* validatedOptionsVtable{};
bool getterValidated{};
CameraPerspective lastPerspective{CameraPerspective::Unknown};
std::atomic<CameraPerspective> currentPerspective{CameraPerspective::Unknown};
SteadyTime lastPublished{};
std::string lastFailure;
bool validatedOnce{};

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

const std::uint8_t* CodeSlot(const void* instance, std::size_t index) noexcept {
    const auto address = reinterpret_cast<std::uintptr_t>(SafeMemory::ReadVirtualSlot(instance, index));
    return address >= textBegin && address < textEnd ? reinterpret_cast<const std::uint8_t*>(address) : nullptr;
}

// ClientInstance::getOptions must be "mov rax, [rcx+disp32]; ret"; returns
// the displacement it reads.
bool OptionsFieldOffset(const void* clientInstance, std::size_t& offset) noexcept {
    const auto* code = CodeSlot(clientInstance, ClientInstance_GetOptionsSlot);
    std::array<std::uint8_t, 8> bytes{};
    if (!code || !SafeMemory::ReadField(code, 0, bytes)) return false;
    if (bytes[0] != 0x48 || bytes[1] != 0x8B || bytes[2] != 0x81 || bytes[7] != 0xC3) return false;
    std::int32_t displacement{};
    std::memcpy(&displacement, bytes.data() + 3, sizeof(displacement));
    if (displacement <= 0 || displacement > 0x4000 || displacement % 8 != 0) return false;
    offset = static_cast<std::size_t>(displacement);
    return true;
}

template<std::size_t N>
bool CodeMatches(const std::uint8_t* code, std::size_t offset, const std::array<std::uint8_t, N>& expected) noexcept {
    std::array<std::uint8_t, N> actual{};
    return SafeMemory::ReadField(code, offset, actual) && actual == expected;
}

// The perspective getter must read option 3, follow +0x238 overrides and
// return the int at +0x18 - exactly what Observe then does by hand:
//   +0x1F  mov r8d, 3
//   +0x37  mov rcx, [rcx+0x238]
//   +0x43  mov eax, [rax+0x18]
bool IsPerspectiveGetter(const std::uint8_t* code) noexcept {
    return code
        && CodeMatches(code, 0x1F, std::array<std::uint8_t, 6>{0x41, 0xB8, 0x03, 0x00, 0x00, 0x00})
        && CodeMatches(code, 0x37, std::array<std::uint8_t, 7>{0x48, 0x8B, 0x89, 0x38, 0x02, 0x00, 0x00})
        && CodeMatches(code, 0x43, std::array<std::uint8_t, 3>{0x8B, 0x40, 0x18});
}

std::string HexBytes(const std::uint8_t* code) {
    std::array<std::uint8_t, 0x50> bytes{};
    if (!code || !SafeMemory::ReadField(code, 0, bytes)) return "unreadable";
    std::string text;
    for (const auto byte : bytes) text += std::format("{:02X} ", byte);
    return text;
}

// The registered perspective option: the right id and telemetry name.
const void* PerspectiveOption(const void* options) noexcept {
    const void* option{};
    const void* impl{};
    std::uint32_t id{};
    if (!SafeMemory::ReadField(options, Options_Table + PerspectiveOptionId * sizeof(void*), option) || !option
        || !SafeMemory::ReadField(option, Option_Impl, impl) || !impl
        || !SafeMemory::ReadField(impl, OptionImpl_Id, id) || id != PerspectiveOptionId) {
        return nullptr;
    }
    std::string name;
    try {
        if (!SafeMemory::ReadMsvcString(impl, OptionImpl_TelemetryName, name, 64) || name != PerspectiveTelemetryName) return nullptr;
    } catch (...) {
        return nullptr;
    }
    return option;
}

bool ReadPerspective(const void* actor, CameraPerspective& perspective, std::string& failure) noexcept {
    const void* clientInstance{};
    if (!SafeMemory::ReadField(actor, Actor_ClientInstance, clientInstance) || !clientInstance) {
        failure = "no ClientInstance on the local player";
        return false;
    }
    std::size_t optionsOffset{};
    if (!OptionsFieldOffset(clientInstance, optionsOffset)) {
        failure = "ClientInstance::getOptions did not match this build";
        return false;
    }
    const void* options{};
    const void* optionsVtable{};
    if (!SafeMemory::ReadField(clientInstance, optionsOffset, options) || !options
        || !SafeMemory::ReadField(options, 0, optionsVtable)) {
        failure = "Options was unreadable";
        return false;
    }
    if (options != validatedOptions || optionsVtable != validatedOptionsVtable) {
        const auto* getter = CodeSlot(options, Options_PerspectiveSlot);
        getterValidated = IsPerspectiveGetter(getter);
        if (!getterValidated) {
            const auto rva = getter ? reinterpret_cast<std::uintptr_t>(getter)
                - reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr)) : 0;
            try {
                Diagnostics::LogLine("warn", "camera_getter_mismatch", std::format(
                    "getPlayerViewPerspective (rva 0x{:X}) did not match; reading the option without overrides. bytes: {}",
                    rva, HexBytes(getter)));
            } catch (...) {}
        }
        validatedOptions = options;
        validatedOptionsVtable = optionsVtable;
    }
    const void* option = PerspectiveOption(options);
    if (!option) {
        failure = "the perspective option failed validation";
        return false;
    }
    // The override hop at +0x238 is only trusted once the getter's own code
    // confirmed it; otherwise the option (already checked by id and
    // telemetry name) is read directly.
    for (std::size_t hop = 0; getterValidated && hop < MaxOverrideHops; ++hop) {
        const void* impl{};
        const void* overriding{};
        if (!SafeMemory::ReadField(option, Option_Impl, impl) || !impl
            || !SafeMemory::ReadField(impl, OptionImpl_Override, overriding)) {
            failure = "an option override was unreadable";
            return false;
        }
        if (!overriding) break;
        option = overriding;
    }
    std::int32_t value{};
    if (!SafeMemory::ReadField(option, IntOption_Value, value) || value < 0 || value > 2) {
        failure = "the perspective value was implausible";
        return false;
    }
    perspective = value == 0 ? CameraPerspective::FirstPerson
        : value == 1 ? CameraPerspective::ThirdPersonBack : CameraPerspective::ThirdPersonFront;
    return true;
}

const char* Name(CameraPerspective perspective) noexcept {
    switch (perspective) {
    case CameraPerspective::FirstPerson: return "first-person";
    case CameraPerspective::ThirdPersonBack: return "third-person-back";
    case CameraPerspective::ThirdPersonFront: return "third-person-front";
    case CameraPerspective::Unknown: break;
    }
    return "unknown";
}

void Publish(ModuleManager& modules, CameraPerspective perspective, SteadyTime now) noexcept {
    const bool changed = perspective != lastPerspective;
    if (!changed && now - lastPublished < RepublishInterval) return;
    if (changed) Diagnostics::LogLine("info", "camera_perspective", Name(perspective));
    lastPerspective = perspective;
    currentPerspective.store(perspective, std::memory_order_relaxed);
    lastPublished = now;
    try { modules.Publish(CameraPerspectiveEvent{now, perspective}); } catch (...) {}
}

}

void Attach(ModuleManager& modules) noexcept {
    validatedOptions = nullptr;
    validatedOptionsVtable = nullptr;
    getterValidated = false;
    lastPerspective = CameraPerspective::Unknown;
    lastPublished = {};
    lastFailure.clear();
    validatedOnce = false;
    if (!ResolveText()) {
        Diagnostics::LogLine("warn", "camera_unavailable", "Minecraft's code section could not be located");
        return;
    }
    moduleManager.store(&modules, std::memory_order_release);
}

CameraPerspective Current() noexcept { return currentPerspective.load(std::memory_order_relaxed); }

void Detach() noexcept {
    currentPerspective.store(CameraPerspective::Unknown, std::memory_order_relaxed);
    auto* modules = moduleManager.exchange(nullptr, std::memory_order_acq_rel);
    if (!modules) return;
    try { modules->Publish(CameraPerspectiveEvent{SteadyClock::now(), CameraPerspective::Unknown}); } catch (...) {}
}

void Observe(const void* actor, SteadyTime now) noexcept {
    auto* modules = moduleManager.load(std::memory_order_acquire);
    if (!modules || !actor) return;
    CameraPerspective perspective{CameraPerspective::Unknown};
    std::string failure;
    if (!ReadPerspective(actor, perspective, failure)) {
        if (failure != lastFailure) {
            Diagnostics::LogLine("warn", "camera_unvalidated", failure);
            lastFailure = failure;
        }
        Publish(*modules, CameraPerspective::Unknown, now);
        return;
    }
    if (!validatedOnce) {
        validatedOnce = true;
        Diagnostics::LogLine("info", "camera_validated", Name(perspective));
    }
    lastFailure.clear();
    if (const auto snapped = PresentationAdapter::PerspectiveOverride()) perspective = *snapped;
    Publish(*modules, perspective, now);
}

}
