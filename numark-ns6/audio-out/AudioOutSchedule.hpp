#pragma once

#include "AudioOutTransferBatch.hpp"

#include <cstdint>

struct AudioOutScheduleResult final {
    std::uint64_t startFrame;
    std::uint32_t skippedIntervals;
};

class AudioOutSchedule final {
public:
    static constexpr std::uint64_t kStartLeadFrames = 2;

    std::uint64_t begin(std::uint64_t currentFrame) noexcept;
    AudioOutScheduleResult takeNext(std::uint64_t currentFrame) noexcept;
    std::uint64_t skippedIntervals() const noexcept { return skippedIntervals_; }

private:
    std::uint64_t nextFrame_ = 0;
    std::uint64_t skippedIntervals_ = 0;
};
