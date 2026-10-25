#include "../Source/PluginAPI.h"
#include <windows.h>
#include <cstring>
#include <iostream>

int main() {
    HMODULE library = LoadLibraryW(L"build/subtitles.dll");
    if (!library) { std::cerr << "LoadLibrary failed " << GetLastError(); return 1; }
    const auto entry = GetProcAddress(library, "EML5_Load");
    using Load = bool(__fastcall*)(PluginInfo*);
    Load load = nullptr;
    static_assert(sizeof load == sizeof entry);
    std::memcpy(&load, &entry, sizeof load);
    if (!load) return 2;
    PluginInfo info{};
    if (load(&info)) { std::cerr << "Unsupported host unexpectedly accepted"; return 3; }
    if (info.name != nullptr || info.infoVersion != 0) return 4;
    FreeLibrary(library);
    std::cout << "PASS DLL loads, exports EML5_Load, and rejects an unsupported host before hook installation\n";
}
