#include "Oinkster/ProcessInjector.hpp"
#include <iostream>

int wmain(int argc, wchar_t** argv) {
    if (argc != 5) {
        std::wcerr << L"Usage: Oinkster.Injector <pid> <absolute-dll-path> <expected-executable> <required-path-marker>\n";
        return 64;
    }
    DWORD processId{};
    try { processId = std::stoul(argv[1]); } catch (...) { return 64; }
    const auto result = Oinkster::ProcessInjector{}.Inject(processId, argv[2], argv[3], argv[4]);
    std::wcout << L"{\"status\":" << static_cast<int>(result.status) << L",\"message\":\"" << result.message << L"\"}\n";
    return result.Succeeded() ? 0 : 1;
}

