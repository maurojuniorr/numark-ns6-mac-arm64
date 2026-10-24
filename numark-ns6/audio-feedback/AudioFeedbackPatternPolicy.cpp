#include "AudioFeedbackPatternPolicy.hpp"

bool AudioFeedbackPatternPolicy::build(
        std::uint8_t framesPerMillisecond,
        std::array<std::uint8_t, kPacketsPerMillisecond>& packets) const noexcept {
    if (framesPerMillisecond < kMinimumFrames || framesPerMillisecond > kMaximumFrames) {
        return false;
    }
    static constexpr std::array<std::array<std::uint8_t, kPacketsPerMillisecond>, 5> patterns{{
        {{6, 5, 5, 5, 6, 5, 5, 5}},
        {{5, 5, 6, 5, 6, 5, 6, 5}},
        {{6, 5, 6, 5, 6, 5, 6, 5}},
        {{6, 6, 5, 6, 5, 6, 5, 6}},
        {{5, 6, 6, 6, 5, 6, 6, 6}},
    }};
    packets = patterns[framesPerMillisecond - kMinimumFrames];
    return true;
}
