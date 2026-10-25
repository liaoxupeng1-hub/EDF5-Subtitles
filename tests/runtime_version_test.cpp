#include <windows.h>
#include <filesystem>
#include <string>
#include <iostream>
#include <cstdlib>
static std::wstring fixturePath;
DWORD WINAPI FixtureModuleName(HMODULE, LPWSTR output, DWORD capacity) {
    if (fixturePath.size() + 1 > capacity) return 0;
    std::copy(fixturePath.begin(), fixturePath.end(), output);
    output[fixturePath.size()] = 0;
    return static_cast<DWORD>(fixturePath.size());
}
// Replace only the source of the EXE path; use the actual file reader, SHA-256
// calculation and allowlist from Runtime.cpp. No process hooks are installed.
#define GetModuleFileNameW FixtureModuleName
#include "../Source/Runtime.cpp"
#undef GetModuleFileNameW
int main() {
    unsigned positiveChecks = 0;
    for (const auto* variable : {L"EDF5_TEST_GAME_EXE", L"EDF5_TEST_OTHER_EXE"}) {
        const auto* path = _wgetenv(variable);
        if (!path || !*path) continue;
        fixturePath = path;
        if (!edf5sub::VerifyExecutable()) { std::cerr << "Known executable rejected\n"; return 1; }
        ++positiveChecks;
    }
    fixturePath = std::filesystem::absolute("build/tests/plugin_load_test.exe").wstring();
    if (edf5sub::VerifyExecutable()) { std::cerr << "Unknown executable accepted\n"; return 2; }
    std::cout << "PASS actual Runtime version guard: unrelated EXE rejected; "
              << positiveChecks << " supplied known executable(s) accepted\n";
    if (!positiveChecks) std::cout << "SKIP optional positive checks: set EDF5_TEST_GAME_EXE / EDF5_TEST_OTHER_EXE\n";
}
