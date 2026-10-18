#include <Windows.h>

int wmain() {
    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, L"Local\\Oinkster.InjectionHost.Stop");
    if (!event) return 1;
    WaitForSingleObject(event, 30000);
    CloseHandle(event);
    return 0;
}

