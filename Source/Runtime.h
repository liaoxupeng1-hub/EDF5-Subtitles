#pragma once
#include "SubtitleModel.h"
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace edf5sub {
struct Config {
    float fontSize = 32;
    Timing timing;
    bool diagnostics = true;
};

const Config& Settings();
bool InitializeRuntime();
void InstallVoiceHooksInTransaction();
bool VoiceHooksReady();
void TakePending(std::vector<SubtitleInstance>& destination);
int PlaybackState(std::uint32_t id);
bool IsPaused();
std::string FontGlyphText();
void Log(const char* format, ...) noexcept;
void LogPlaybackEnd(const SubtitleInstance& item) noexcept;
}
