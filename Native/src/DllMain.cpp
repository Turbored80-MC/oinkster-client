#include "Oinkster/Runtime.hpp"
#include "Oinkster/UnloadGuard.hpp"
#include <Windows.h>
#include <memory>

namespace {
HMODULE Module{};

DWORD WINAPI Bootstrap(void*) noexcept {
    wchar_t localAppData[MAX_PATH]{};
    DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH);
    const auto root = length > 0 && length < MAX_PATH
        ? std::filesystem::path(localAppData) / "Oinkster" / "Client"
        : std::filesystem::temp_directory_path() / "Oinkster";
    int exitCode = 1;
    // Scoped so ~Runtime (and with it ~StatusPipe/~ControlPipe and the module
    // vector) actually runs: FreeLibraryAndExitThread never returns, so a
    // Runtime left live at this scope would never be destroyed.
    {
        Oinkster::Runtime runtime(root);
        exitCode = runtime.Run();
    }
    if (Oinkster::UnloadGuard::Blocked()) {
        // Some part of our code is still reachable from the game (a trampoline
        // with a live thread in it, or a window we failed to un-subclass).
        // Unmapping the image now would crash Minecraft, so leak it instead
        // and just retire this thread.
        ExitThread(static_cast<DWORD>(exitCode));
    }
    FreeLibraryAndExitThread(Module, static_cast<DWORD>(exitCode));
}
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void*) {
    if (reason == DLL_PROCESS_ATTACH) {
        Module = instance;
        DisableThreadLibraryCalls(instance);
        HANDLE thread = CreateThread(nullptr, 0, Bootstrap, nullptr, 0, nullptr);
        if (!thread) return FALSE;
        CloseHandle(thread);
    }
    return TRUE;
}

