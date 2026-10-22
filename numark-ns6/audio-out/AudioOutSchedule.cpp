#include "AudioOutSchedule.hpp"

#include <limits>

std::uint64_t AudioOutSchedule::begin(std::uint64_t currentFrame) noexcept {
    constexpr std::uint64_t batchFrames = AudioOutTransferBatch::kServiceIntervals / 8;
    if (currentFrame > std::numeric_limits<std::uint64_t>::max() - kStartLeadFrames) {
        return 0;
    }
    nextFrame_ = currentFrame + kStartLeadFrames;
    if (nextFrame_ > std::numeric_limits<std::uint64_t>::max() -
                                  batchFrames) {
        nextFrame_ = 0;
        return 0;
    }
    skippedIntervals_ = 0;
    const auto first = nextFrame_;
    nextFrame_ += batchFrames;
    return first;
}

AudioOutScheduleResult AudioOutSchedule::takeNext(std::uint64_t currentFrame) noexcept {
    constexpr std::uint64_t batchFrames = AudioOutTransferBatch::kServiceIntervals / 8;
    const std::uint64_t lead = kStartLeadFrames;
    if (currentFrame > std::numeric_limits<std::uint64_t>::max() - lead) return {0, 0};
    const std::uint64_t minimum = currentFrame + lead;
    std::uint32_t skipped = 0;
    if (nextFrame_ < minimum) {
        const std::uint64_t behind = minimum - nextFrame_;
        const std::uint64_t interval = batchFrames;
        const std::uint64_t count = behind / interval + (behind % interval != 0);
        if (count > std::numeric_limits<std::uint32_t>::max()) return {0, 0};
        skipped = static_cast<std::uint32_t>(count);
        if (count > (std::numeric_limits<std::uint64_t>::max() - nextFrame_) / interval) {
            return {0, 0};
        }
        nextFrame_ += count * interval;
        skippedIntervals_ += count;
    }
    const std::uint64_t start = nextFrame_;
    if (start > std::numeric_limits<std::uint64_t>::max() -
                        batchFrames) {
        return {0, 0};
    }
    nextFrame_ += batchFrames;
    return {start, skipped};
}
