#include "Runtime.h"
#include "SubtitleTable.h"
#include <bcrypt.h>
#include <detours.h>
#include <array>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

namespace edf5sub {
namespace {
Config config;
SubtitleTable table;
std::mutex queueMutex, logMutex;
std::vector<SubtitleInstance> pending;
std::atomic<bool> paused{false};
std::atomic<std::uint64_t> nextSerial{1};
std::uintptr_t imageBase = 0;
bool hooksReady = false;
FILE* logFile = nullptr;

struct GameString {
    union { char data[16]; const char* ptr; };
    std::uint64_t length, capacity;
};
static_assert(sizeof(GameString) == 32);
using PlayFn = bool(__fastcall*)(void*, GameString*, void*);
using SurroundFn = bool(__fastcall*)(void*, GameString*, void*, void*, int, unsigned);
using CreateFn = void*(__fastcall*)(void*, const void*);
using StateFn = int(__fastcall*)(std::uint32_t);
using PauseFn = void*(__fastcall*)(void*, void*);
using UnpauseFn = void*(__fastcall*)(void*, unsigned);
PlayFn originalPlay;
SurroundFn originalSurround;
CreateFn originalCreate;
StateFn nativeState;
PauseFn originalPause;
UnpauseFn originalUnpause;

struct Capture {
    Capture* parent;
    std::string name;
    std::array<std::uint32_t, 32> ids{};
    std::size_t count = 0;
    bool spatial = false;
};
thread_local Capture* currentCapture = nullptr;
struct CaptureScope {
    Capture& capture;
    explicit CaptureScope(Capture& c) : capture(c) { currentCapture = &capture; }
    ~CaptureScope() { currentCapture = capture.parent; }
};

bool ReadMemory(const void* at, void* out, SIZE_T count) noexcept {
    SIZE_T read = 0;
    return at && ReadProcessMemory(GetCurrentProcess(), at, out, count, &read) && read == count;
}

std::string VoiceName(const GameString* source) {
    GameString copy{};
    if (!ReadMemory(source, &copy, sizeof copy) || !copy.length || copy.length > 128 || copy.capacity < copy.length)
        return {};
    std::string value(static_cast<std::size_t>(copy.length), '\0');
    const void* data = copy.capacity > 15 ? static_cast<const void*>(copy.ptr) : static_cast<const void*>(source);
    if (!ReadMemory(data, value.data(), value.size())) return {};
    for (unsigned char c : value)
        if (c < 0x21 || c > 0x7e) return {};
    return value;
}

void Submit(const Capture& capture) noexcept {
    try {
        const auto* record = table.Find(capture.name);
        if (!record) { Log("lookup_miss voice=%s", capture.name.c_str()); return; }
        if (!capture.count) { Log("capture_missing voice=%s (not displayed)", capture.name.c_str()); return; }
        SubtitleInstance item;
        item.serial = nextSerial.fetch_add(1);
        item.voiceId = capture.name;
        item.caption = DisplayCaption(record->caption, capture.spatial);
        item.playbackIds.assign(capture.ids.begin(), capture.ids.begin() + capture.count);
        std::lock_guard<std::mutex> lock(queueMutex);
        if (pending.size() >= 512) { Log("queue_capacity reached"); return; }
        Log("play serial=%llu voice=%s handles=%zu first=%08x",
            static_cast<unsigned long long>(item.serial), item.voiceId.c_str(), item.playbackIds.size(), item.playbackIds.front());
        pending.push_back(std::move(item));
    } catch (...) { Log("subtitle submission allocation failure"); }
}

void* __fastcall HookCreate(void* bank, const void* request) {
    void* result = originalCreate(bank, request);
    if (result && currentCapture) {
        std::uint32_t id = 0xffffffffu;
        if (ReadMemory(static_cast<const char*>(result) + 0x10, &id, sizeof id) && id != 0xffffffffu) {
            auto& capture = *currentCapture;
            bool duplicate = false;
            for (std::size_t i = 0; i < capture.count; ++i) duplicate |= capture.ids[i] == id;
            if (!duplicate && capture.count < capture.ids.size()) capture.ids[capture.count++] = id;
        }
    }
    return result;
}

bool __fastcall HookPlay(void* manager, GameString* name, void* output) {
    Capture capture{};
    capture.parent = currentCapture;
    try { capture.name = VoiceName(name); } catch (...) {}
    bool result;
    {
        CaptureScope scope(capture);
        result = originalPlay(manager, name, output);
    }
    if (result && !capture.name.empty()) Submit(capture);
    return result;
}

bool __fastcall HookSurround(void* manager, GameString* name, void* spatial, void* output, int a5, unsigned a6) {
    Capture capture{};
    capture.parent = currentCapture;
    capture.spatial = true;
    try { capture.name = VoiceName(name); } catch (...) {}
    bool result;
    {
        CaptureScope scope(capture);
        result = originalSurround(manager, name, spatial, output, a5, a6);
    }
    if (result && !capture.name.empty()) Submit(capture);
    return result;
}

void* __fastcall HookPause(void* a, void* b) {
    void* result = originalPause(a, b);
    paused.store(true);
    return result;
}
void* __fastcall HookUnpause(void* a, unsigned b) {
    void* result = originalUnpause(a, b);
    paused.store(false);
    return result;
}

bool VerifyExecutable() {
    wchar_t path[32768]{};
    if (!GetModuleFileNameW(nullptr, path, 32768)) return false;
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::array<unsigned char, 32> digest{};
    bool okay = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0;
    if (okay) okay = BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) >= 0;
    std::array<unsigned char, 65536> buffer{};
    DWORD read = 0;
    while (okay) {
        if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) { okay = false; break; }
        if (!read) break;
        okay = BCryptHashData(hash, buffer.data(), read, 0) >= 0;
    }
    if (okay) okay = BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0;
    if (hash) BCryptDestroyHash(hash);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    CloseHandle(file);
    constexpr std::array<unsigned char, 32> originalAnalyzed = {
        0x6c,0x2e,0x54,0xfe,0x48,0xea,0xd4,0xfd,0xc8,0x0b,0x1e,0xc3,0x47,0x33,0xe5,0x20,
        0x44,0xbd,0xa3,0xdf,0x8e,0x2f,0xc0,0xaf,0xc9,0xe6,0xf6,0xa9,0x3f,0x23,0xf7,0xf6};
    // The reinstalled executable differs elsewhere, but all eight critical
    // playback, handle/state and pause ranges were compared byte for byte.
    constexpr std::array<unsigned char, 32> reinstalled = {
        0x35,0x12,0xd2,0xa2,0xe6,0x1d,0x53,0x2c,0x5d,0x12,0xdc,0x0c,0x5f,0xd1,0xac,0x61,
        0xf6,0x74,0x3e,0x13,0x34,0x9a,0xe2,0x13,0x01,0xbb,0x84,0x49,0xdd,0x4b,0xae,0x5f};
    return okay && (digest == originalAnalyzed || digest == reinstalled);
}

void LoadConfig() {
    std::ifstream file("Mods/Plugins/subtitles.toml");
    std::string line;
    while (std::getline(file, line)) {
        line = line.substr(0, line.find('#'));
        auto equal = line.find('=');
        if (equal == std::string::npos) continue;
        auto key = line.substr(0, equal);
        key.erase(std::remove_if(key.begin(), key.end(), [](unsigned char c) { return c == ' ' || c == '\t' || c == '\r'; }), key.end());
        std::istringstream value(line.substr(equal + 1));
        double number;
        if (key == "diagnostics") {
            std::string word; value >> word; config.diagnostics = word == "true";
        } else if (value >> number && std::isfinite(number)) {
            if (key == "font_size") config.fontSize = static_cast<float>(std::clamp(number, 12.0, 96.0));
            if (key == "post_voice_hold_seconds") config.timing.holdAfterVoice = std::clamp(number, 0.0, 10.0);
            if (key == "fade_out_seconds") config.timing.fadeOut = std::clamp(number, 0.05, 5.0);
        }
    }
}
} // namespace

const Config& Settings() { return config; }

void Log(const char* format, ...) noexcept {
    try {
        if (!logFile) return;
        std::lock_guard<std::mutex> lock(logMutex);
        std::fprintf(logFile, "[%llu] ", static_cast<unsigned long long>(GetTickCount64()));
        va_list args; va_start(args, format); std::vfprintf(logFile, format, args); va_end(args);
        std::fputc('\n', logFile); std::fflush(logFile);
    } catch (...) {}
}

bool InitializeRuntime() {
    LoadConfig();
    if (config.diagnostics) logFile = std::fopen("Mods/Plugins/subtitles-test.log", "w");
    Log("EDF5 subtitles 1.2.0-test2 candidate; native playback tracking");
    if (!VerifyExecutable()) { Log("unsupported EDF5 executable; hooks not installed"); return false; }
    std::string error;
    if (!table.LoadFile("Mods/MISSION/EDF6_VOICETABLE.SUBTITLE.SGO", &error)) {
        Log("subtitle table failed: %s; hooks not installed", error.c_str()); return false;
    }
    imageBase = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    originalPlay = reinterpret_cast<PlayFn>(imageBase + 0x47c9d0);
    originalSurround = reinterpret_cast<SurroundFn>(imageBase + 0x47cba0);
    originalCreate = reinterpret_cast<CreateFn>(imageBase + 0x477c50);
    nativeState = reinterpret_cast<StateFn>(imageBase + 0x92b148);
    originalPause = reinterpret_cast<PauseFn>(imageBase + 0x585fc0);
    originalUnpause = reinterpret_cast<UnpauseFn>(imageBase + 0x5869e0);
    Log("verified executable; loaded %zu subtitles; hold=%.3f fade=%.3f font=%.1f",
        table.Size(), config.timing.holdAfterVoice, config.timing.fadeOut, config.fontSize);
    return true;
}

void InstallVoiceHooksInTransaction() {
    hooksReady = DetourAttach(reinterpret_cast<PVOID*>(&originalPlay), reinterpret_cast<PVOID>(HookPlay)) == NO_ERROR;
    hooksReady &= DetourAttach(reinterpret_cast<PVOID*>(&originalSurround), reinterpret_cast<PVOID>(HookSurround)) == NO_ERROR;
    hooksReady &= DetourAttach(reinterpret_cast<PVOID*>(&originalCreate), reinterpret_cast<PVOID>(HookCreate)) == NO_ERROR;
    hooksReady &= DetourAttach(reinterpret_cast<PVOID*>(&originalPause), reinterpret_cast<PVOID>(HookPause)) == NO_ERROR;
    hooksReady &= DetourAttach(reinterpret_cast<PVOID*>(&originalUnpause), reinterpret_cast<PVOID>(HookUnpause)) == NO_ERROR;
}
bool VoiceHooksReady() { return hooksReady; }

void TakePending(std::vector<SubtitleInstance>& destination) {
    std::lock_guard<std::mutex> lock(queueMutex);
    for (auto& item : pending) destination.push_back(std::move(item));
    pending.clear();
}
int PlaybackState(std::uint32_t id) { return nativeState ? nativeState(id) : 3; }
bool IsPaused() { return paused.load(); }
std::string FontGlyphText() {
    std::string text;
    table.ForEachRecord([&](const SubtitleRecord& record) {
        text += record.caption;
        text += record.name;
    });
    return text;
}
void LogPlaybackEnd(const SubtitleInstance& item) noexcept {
    Log("ended serial=%llu voice=%s displayed=%.3fs",
        static_cast<unsigned long long>(item.serial), item.voiceId.c_str(), item.elapsed);
}
} // namespace edf5sub
