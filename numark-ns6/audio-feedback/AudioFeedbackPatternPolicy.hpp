#pragma once

#include <array>
#include <cstdint>

class AudioFeedbackPatternPolicy final {
public:
    static constexpr std::uint32_t kPacketsPerMillisecond = 8;
    static constexpr std::uint8_t kMinimumFrames = 42;
    static constexpr std::uint8_t kMaximumFrames = 46;

    bool build(std::uint8_t framesPerMillisecond,
               std::array<std::uint8_t, kPacketsPerMillisecond>& packets) const noexcept;
};
