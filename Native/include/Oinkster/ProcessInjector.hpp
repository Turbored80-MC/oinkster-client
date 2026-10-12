#pragma once

#include <Windows.h>
#include <chrono>
#include <filesystem>
#include <string>

namespace Oinkster {

enum class ProcessInjectionStatus {
    Success,
    InvalidArgument,
    TargetMismatch,
    ArchitectureMismatch,
    AccessDenied,
    AllocationFailed,
    WriteFailed,
    LoadLibraryUnavailable,
    ThreadCreationFailed,
    TimedOut,
    LoadFailed
};

struct ProcessInjectionResult {
    ProcessInjectionStatus status{};
    std::wstring message;
    [[nodiscard]] bool Succeeded() const noexcept { return status == ProcessInjectionStatus::Success; }
};

class ProcessInjector final {
public:
    [[nodiscard]] ProcessInjectionResult Inject(
        DWORD processId,
        const std::filesystem::path& dllPath,
        std::wstring_view expectedExecutable,
        std::wstring_view requiredPathMarker,
        std::chrono::milliseconds timeout = std::chrono::seconds(10)) const noexcept;
};

}

