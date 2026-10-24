#include "AudioPacketPolicy.hpp"
#include "../audio-feedback/AudioFeedbackPatternPolicy.hpp"

#include <algorithm>
#include <stdexcept>

namespace {
constexpr std::uint64_t kMinimumOutputRateMilliHz = 44100000;
constexpr std::uint64_t kMaximumOutputRateMilliHz = 44100300;
constexpr std::uint32_t kQueueTargetFrames = 4096;
}

AudioPacketPolicy::AudioPacketPolicy(std::uint32_t sampleRateHz,
                                     std::uint32_t serviceIntervalsPerSecond)
        : wholeFrames_(serviceIntervalsPerSecond == 0 ? 0 : sampleRateHz / serviceIntervalsPerSecond),
          remainderFrames_(serviceIntervalsPerSecond == 0 ? 0 : sampleRateHz % serviceIntervalsPerSecond),
          serviceIntervalsPerSecond_(serviceIntervalsPerSecond), sampleRateHz_(sampleRateHz),
          measuredClockMilliHz_(static_cast<std::uint64_t>(sampleRateHz) * 1000) {
    if (sampleRateHz == 0 || serviceIntervalsPerSecond == 0) {
        throw std::invalid_argument("sample rate and USB service rate must be nonzero");
    }
    AudioFeedbackPatternPolicy{}.build(44, feedbackPattern_);
    for (std::size_t i = 0; i < patternHistory_.size(); ++i)
        patternHistory_[i] = static_cast<std::uint8_t>(5 + (i % 2) + (i % 80 == 0));
}

std::uint32_t AudioPacketPolicy::nextPacketFrames() noexcept {
    if (serviceIntervalsPerSecond_ == 8000) {
        if (patternIndex_ == 0) {
            if (feedbackQueueCount_ != 0) {
                const auto feedback = feedbackQueue_[feedbackQueueRead_];
                feedbackQueueRead_ = (feedbackQueueRead_ + 1) % feedbackQueue_.size();
                --feedbackQueueCount_;
                std::array<std::uint8_t, 8> selected{};
                if (AudioFeedbackPatternPolicy{}.build(feedback, selected)) {
                    feedbackPattern_ = selected;
                    for (std::size_t i = 0; i < selected.size(); ++i)
                        patternHistory_[(patternHistoryCursor_ + i) % patternHistory_.size()] = selected[i];
                    patternHistoryCursor_ = (patternHistoryCursor_ + selected.size()) % patternHistory_.size();
                    if (patternHistoryCursor_ == reacquisitionReference_) patternLocked_ = true;
                } else {
                    patternLocked_ = false;
                    reacquisitionReference_ = patternHistoryCursor_;
                    for (std::size_t i = 0; i < feedbackPattern_.size(); ++i)
                        feedbackPattern_[i] = patternHistory_[(patternHistoryCursor_ + i) % patternHistory_.size()];
                    for (std::size_t i = 0; i < feedbackPattern_.size(); ++i)
                        patternHistory_[(patternHistoryCursor_ + i) % patternHistory_.size()] = feedbackPattern_[i];
                    patternHistoryCursor_ = (patternHistoryCursor_ + feedbackPattern_.size()) % patternHistory_.size();
                }
            } else {
                nominalFraction_ += 100;
                const auto framesPerMillisecond = static_cast<std::uint8_t>(
                        44 + (nominalFraction_ >= 1000 ? 1 : 0));
                if (nominalFraction_ >= 1000) nominalFraction_ -= 1000;
                AudioFeedbackPatternPolicy{}.build(framesPerMillisecond, feedbackPattern_);
            }
        }
        const auto frames = feedbackPattern_[patternIndex_];
        patternIndex_ = static_cast<std::uint8_t>((patternIndex_ + 1) % feedbackPattern_.size());
        return frames;
    }
    std::uint32_t frames = wholeFrames_;
    phase_ += remainderFrames_;
    if (phase_ >= serviceIntervalsPerSecond_) {
        phase_ -= serviceIntervalsPerSecond_;
        ++frames;
    }
    return frames;
}

void AudioPacketPolicy::observeFeedback(std::uint8_t framesPerMillisecond) noexcept {
    if (framesPerMillisecond >= AudioFeedbackPatternPolicy::kMinimumFrames &&
        framesPerMillisecond <= AudioFeedbackPatternPolicy::kMaximumFrames) {
        if (feedbackCount_ == kFeedbackWindow)
            feedbackSum_ -= feedbackWindow_[feedbackIndex_];
        else
            ++feedbackCount_;
        feedbackWindow_[feedbackIndex_] = framesPerMillisecond;
        feedbackSum_ += framesPerMillisecond;
        feedbackIndex_ = (feedbackIndex_ + 1) % kFeedbackWindow;
        if (feedbackCount_ == kFeedbackWindow)
            measuredClockMilliHz_ = feedbackSum_ * 1000000 / kFeedbackWindow;

        if (feedbackQueueCount_ == feedbackQueue_.size()) {
            feedbackQueueRead_ = (feedbackQueueRead_ + 1) % feedbackQueue_.size();
            --feedbackQueueCount_;
        }
        const auto write = (feedbackQueueRead_ + feedbackQueueCount_) % feedbackQueue_.size();
        feedbackQueue_[write] = framesPerMillisecond;
        ++feedbackQueueCount_;
    }
}

void AudioPacketPolicy::observeInvalidFeedback() noexcept {
    if (feedbackQueueCount_ == feedbackQueue_.size()) {
        feedbackQueueRead_ = (feedbackQueueRead_ + 1) % feedbackQueue_.size();
        --feedbackQueueCount_;
    }
    const auto write = (feedbackQueueRead_ + feedbackQueueCount_) % feedbackQueue_.size();
    feedbackQueue_[write] = 0;
    ++feedbackQueueCount_;
}

std::uint64_t AudioPacketPolicy::effectiveClockMilliHz() const noexcept {
    return std::clamp(measuredClockMilliHz_, kMinimumOutputRateMilliHz,
                      kMaximumOutputRateMilliHz);
}

double AudioPacketPolicy::sourceFramesPerOutputFrame(std::uint32_t queuedFrames) const noexcept {
    double step = static_cast<double>(sampleRateHz_) * 1000.0 /
                  static_cast<double>(effectiveClockMilliHz());
    double trim = (static_cast<double>(queuedFrames) - kQueueTargetFrames) * 0.000002;
    trim = std::clamp(trim, -0.005, 0.005);
    return std::clamp(step * (1.0 + trim), 0.995, 1.005);
}
