#include "Oinkster/ProcessInjector.hpp"
#include <appmodel.h>
#include <cwchar>
#include <iterator>
#include <memory>

namespace Oinkster {
namespace {
struct HandleCloser { void operator()(void* handle) const noexcept { if (handle && handle != INVALID_HANDLE_VALUE) CloseHandle(handle); } };
using UniqueHandle = std::unique_ptr<void, HandleCloser>;
struct RemoteMemory {
    HANDLE process{};
    void* address{};
    ~RemoteMemory() { if (process && address) VirtualFreeEx(process, address, 0, MEM_RELEASE); }
};

ProcessInjectionResult Error(ProcessInjectionStatus status, const wchar_t* message) { return { status, message }; }
}

ProcessInjectionResult ProcessInjector::Inject(
    DWORD processId,
    const std::filesystem::path& dllPath,
    std::wstring_view expectedExecutable,
    std::wstring_view requiredPathMarker,
    std::chrono::milliseconds timeout) const noexcept {
    try {
        if (processId == 0 || !dllPath.is_absolute() || !std::filesystem::is_regular_file(dllPath))
            return Error(ProcessInjectionStatus::InvalidArgument, L"The process id or DLL path is invalid.");

        UniqueHandle query(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId));
        if (!query) return Error(ProcessInjectionStatus::AccessDenied, L"The target process cannot be inspected.");
        wchar_t targetPath[32768]{};
        DWORD targetLength = static_cast<DWORD>(std::size(targetPath));
        const bool pathAvailable = QueryFullProcessImageNameW(query.get(), 0, targetPath, &targetLength) != FALSE;
        const std::filesystem::path executable = pathAvailable ? std::filesystem::path(targetPath) : std::filesystem::path{};

        std::wstring packageName;
        UINT32 packageLength{};
        if (GetPackageFullName(query.get(), &packageLength, nullptr) == ERROR_INSUFFICIENT_BUFFER) {
            packageName.resize(packageLength);
            if (GetPackageFullName(query.get(), &packageLength, packageName.data()) != ERROR_SUCCESS) {
                packageName.clear();
            }
        }

        const bool packageIsMinecraft = packageName.starts_with(L"Microsoft.MinecraftUWP_");
        const bool executableMatches = pathAvailable
            ? _wcsicmp(executable.filename().c_str(), std::wstring(expectedExecutable).c_str()) == 0
            : packageIsMinecraft && _wcsicmp(std::wstring(expectedExecutable).c_str(), L"Minecraft.Windows.exe") == 0;
        const bool packageMatches = requiredPathMarker.empty()
            || (pathAvailable && executable.wstring().contains(requiredPathMarker))
            || (packageIsMinecraft && packageName.contains(requiredPathMarker));
        if (!executableMatches || !packageMatches)
            return { ProcessInjectionStatus::TargetMismatch,
                L"The target identity does not match. Observed path: " + executable.wstring()
                    + L"; package: " + packageName };

        USHORT processMachine{}, nativeMachine{};
        if (!IsWow64Process2(query.get(), &processMachine, &nativeMachine)
            || (processMachine != IMAGE_FILE_MACHINE_UNKNOWN && processMachine != IMAGE_FILE_MACHINE_AMD64)
            || nativeMachine != IMAGE_FILE_MACHINE_AMD64)
            return Error(ProcessInjectionStatus::ArchitectureMismatch, L"The target is not a native x64 process.");
        query.reset();

        constexpr DWORD Rights = PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ;
        UniqueHandle process(OpenProcess(Rights, FALSE, processId));
        if (!process) return Error(ProcessInjectionStatus::AccessDenied, L"The target denied injection access.");

        const auto dll = dllPath.wstring();
        const SIZE_T byteCount = (dll.size() + 1) * sizeof(wchar_t);
        RemoteMemory remote{ process.get(), VirtualAllocEx(process.get(), nullptr, byteCount, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE) };
        if (!remote.address) return Error(ProcessInjectionStatus::AllocationFailed, L"Remote path allocation failed.");
        SIZE_T written{};
        if (!WriteProcessMemory(process.get(), remote.address, dll.c_str(), byteCount, &written) || written != byteCount)
            return Error(ProcessInjectionStatus::WriteFailed, L"The DLL path could not be written to the target.");

        const HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
        const auto loadLibrary = kernel32 ? GetProcAddress(kernel32, "LoadLibraryW") : nullptr;
        if (!loadLibrary) return Error(ProcessInjectionStatus::LoadLibraryUnavailable, L"LoadLibraryW could not be resolved.");
        UniqueHandle thread(CreateRemoteThread(process.get(), nullptr, 0,
            reinterpret_cast<LPTHREAD_START_ROUTINE>(loadLibrary), remote.address, 0, nullptr));
        if (!thread) return Error(ProcessInjectionStatus::ThreadCreationFailed, L"The target rejected the loader thread.");
        if (WaitForSingleObject(thread.get(), static_cast<DWORD>(timeout.count())) != WAIT_OBJECT_0)
            return Error(ProcessInjectionStatus::TimedOut, L"The loader thread timed out.");
        DWORD result{};
        if (!GetExitCodeThread(thread.get(), &result) || result == 0)
            return Error(ProcessInjectionStatus::LoadFailed, L"Windows did not load the DLL.");
        return { ProcessInjectionStatus::Success, L"The verified DLL was loaded." };
    } catch (...) {
        return Error(ProcessInjectionStatus::InvalidArgument, L"Injector validation failed.");
    }
}

}
