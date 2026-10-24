#pragma once

#include <cstdint>
#include <array>

class AudioPacketPolicy final {
public:
    static constexpr std::size_t kFeedbackWindow = 40000;

    AudioPacketPolicy(std::uint32_t sampleRateHz, std::uint32_t serviceIntervalsPerSecond);

    void observeFeedback(std::uint8_t framesPerMillisecond) noexcept;
    void observeInvalidFeedback() noexcept;
    std::uint32_t nextPacketFrames() noexcept;
    double sourceFramesPerOutputFrame(std::uint32_t queuedFrames) const noexcept;
    std::uint64_t measuredClockMilliHz() const noexcept { return measuredClockMilliHz_; }
    std::uint64_t effectiveClockMilliHz() const noexcept;
    bool patternLocked() const noexcept { return patternLocked_; }

private:
    const std::uint32_t wholeFrames_;
    const std::uint32_t remainderFrames_;
    const std::uint32_t serviceIntervalsPerSecond_;
    const std::uint32_t sampleRateHz_;
    std::uint64_t phase_ = 0;
    std::array<std::uint8_t, 8> feedbackPattern_{};
    // USB reader and output completions are serialized on the worker run loop.
    static constexpr std::size_t kPatternHistorySize = 128;
    std::array<std::uint8_t, kPatternHistorySize> patternHistory_{};
    std::size_t patternHistoryCursor_ = 0;
    std::size_t reacquisitionReference_ = 0;
    bool patternLocked_ = false;
    static constexpr std::size_t kFeedbackQueueCapacity = 32;
    std::array<std::uint8_t, kFeedbackQueueCapacity> feedbackQueue_{};
    std::size_t feedbackQueueRead_ = 0;
    std::size_t feedbackQueueCount_ = 0;
    std::array<std::uint8_t, kFeedbackWindow> feedbackWindow_{};
    std::uint64_t feedbackSum_ = 0;
    std::size_t feedbackCount_ = 0;
    std::size_t feedbackIndex_ = 0;
    std::uint64_t measuredClockMilliHz_;
    std::uint8_t patternIndex_ = 0;
    std::uint32_t nominalFraction_ = 0;
};
