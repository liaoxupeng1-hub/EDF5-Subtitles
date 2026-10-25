#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace edf5sub {

inline std::string DisplayCaption(const std::string& caption, bool spatial) {
    return spatial ? u8"「" + caption + u8"」" : u8"『" + caption + u8"』";
}

struct Timing {
    double holdAfterVoice = 1.0;
    double fadeOut = 1.0;
};

// One entry represents one successful playback request, never a voice-name timer.
struct SubtitleInstance {
    std::uint64_t serial = 0;
    std::string voiceId;
    std::string caption;
    std::vector<std::uint32_t> playbackIds;
    double elapsed = 0;
    double afterVoice = 0;
    bool ended = false;
    float y = -1;

    template<class Query>
    void Advance(double seconds, bool paused, const Timing& timing, Query query) {
        if (paused) return;
        seconds = std::isfinite(seconds) ? std::max(seconds, 0.0) : 0.0;
        elapsed += seconds;
        if (!ended) {
            bool active = false;
            for (const auto id : playbackIds) active |= query(id) != 3;
            if (active) return;
            // Start the exit clock at the observed end; never charge a whole
            // frame interval that could have been spent playing the voice.
            ended = true;
            afterVoice = 0;
            return;
        }
        afterVoice += seconds;
        (void)timing;
    }

    float Alpha(const Timing& timing) const {
        if (!ended || afterVoice <= timing.holdAfterVoice) return 1;
        if (timing.fadeOut <= 0) return 0;
        return static_cast<float>(std::clamp(
            1.0 - (afterVoice - timing.holdAfterVoice) / timing.fadeOut, 0.0, 1.0));
    }

    bool Finished(const Timing& timing) const {
        return ended && afterVoice >= timing.holdAfterVoice + timing.fadeOut;
    }
};

struct ReferenceLayout {
    static constexpr float width = 1920;
    static constexpr float height = 1080;
    static constexpr float wrapWidth = 769.2307739f;
    static constexpr float minBlockWidth = 480;
    static constexpr float maxRowHeight = 128;
    static constexpr float cornerRadius = 16;
    static constexpr float scrollSpeed = 10;

    static float Top(float screenHeight) { return screenHeight * (2.0f / 3.0f); }
    static float Scale(float screenHeight) { return screenHeight / height; }
    static float RowHeight(float contentHeight) {
        return std::min(maxRowHeight, contentHeight);
    }
    static float Scroll(float contentHeight, std::size_t utf16Length, double elapsed) {
        if (contentHeight <= maxRowHeight) return 0;
        const double start = (static_cast<double>(utf16Length) / contentHeight) * 13.0;
        const double distance = contentHeight - maxRowHeight;
        return static_cast<float>(std::clamp((elapsed - start) * scrollSpeed, 0.0, distance));
    }
};

} // namespace edf5sub
