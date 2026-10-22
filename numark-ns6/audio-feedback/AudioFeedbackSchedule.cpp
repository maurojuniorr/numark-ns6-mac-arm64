#include "AudioFeedbackSchedule.hpp"

#include <limits>

std::uint64_t AudioFeedbackSchedule::begin(std::uint64_t currentFrame) noexcept {
    if (currentFrame > std::numeric_limits<std::uint64_t>::max() - kInitialLeadFrames) {
        return 0;
    }
    const std::uint64_t first = currentFrame + kInitialLeadFrames;
    if (first > std::numeric_limits<std::uint64_t>::max() - kFramesPerTransfer) return 0;
    nextFrame_ = first + kFramesPerTransfer;
    return first;
}

AudioFeedbackScheduleResult AudioFeedbackSchedule::takeNext(
        std::uint64_t currentFrame) noexcept {
    if (currentFrame == std::numeric_limits<std::uint64_t>::max()) return {};
    const std::uint64_t minimum = currentFrame + 1;
    std::uint64_t skipped = 0;
    if (nextFrame_ < minimum) {
        const std::uint64_t behind = minimum - nextFrame_;
        skipped = (behind - 1) / kFramesPerTransfer + 1;
        if (skipped > (std::numeric_limits<std::uint64_t>::max() - nextFrame_) /
                              kFramesPerTransfer) {
            return {};
        }
        nextFrame_ += skipped * kFramesPerTransfer;
    }
    if (nextFrame_ > std::numeric_limits<std::uint64_t>::max() - kFramesPerTransfer ||
        skipped > std::numeric_limits<std::uint32_t>::max()) {
        return {};
    }
    const std::uint64_t start = nextFrame_;
    nextFrame_ += kFramesPerTransfer;
    return {start, static_cast<std::uint32_t>(skipped)};
}
