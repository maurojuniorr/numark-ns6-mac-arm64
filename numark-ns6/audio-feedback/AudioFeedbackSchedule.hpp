#pragma once

#include <cstdint>

struct AudioFeedbackScheduleResult final {
    std::uint64_t startFrame = 0;
    std::uint32_t skippedIntervals = 0;
};

class AudioFeedbackSchedule final {
public:
    static constexpr std::uint64_t kInitialLeadFrames = 2;
    static constexpr std::uint64_t kFramesPerTransfer = 4;

    std::uint64_t begin(std::uint64_t currentFrame) noexcept;
    AudioFeedbackScheduleResult takeNext(std::uint64_t currentFrame) noexcept;

private:
    std::uint64_t nextFrame_ = 0;
};
