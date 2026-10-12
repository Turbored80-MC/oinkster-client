#include <Windows.h>

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void*) {
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(instance);
    return TRUE;
}

