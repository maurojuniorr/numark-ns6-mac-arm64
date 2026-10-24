#include "AudioOutputRing.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

static_assert(std::atomic<std::uint32_t>::is_always_lock_free,
              "The audio ring requires lock-free 32-bit atomics");

AudioOutputRing::AudioOutputRing(std::uint32_t capacityFrames, std::uint32_t channels)
        : capacityFrames_(capacityFrames), channels_(channels) {
    if (capacityFrames == 0 || capacityFrames > (std::numeric_limits<std::uint32_t>::max() / 2) ||
            channels == 0 || channels > std::numeric_limits<std::size_t>::max() / capacityFrames) {
        throw std::invalid_argument("invalid audio ring dimensions");
    }
    samples_.resize(static_cast<std::size_t>(capacityFrames) * channels);
}

std::uint32_t AudioOutputRing::write(const float* interleaved, std::uint32_t frames) noexcept {
    if (interleaved == nullptr || frames == 0) return 0;

    const std::uint32_t writeFrame = writeFrame_.load(std::memory_order_relaxed);
    const std::uint32_t readFrame = readFrame_.load(std::memory_order_acquire);
    const std::uint32_t used = writeFrame - readFrame;
    const std::uint32_t count = std::min(frames, capacityFrames_ - used);
    const std::uint32_t offset = writeFrame % capacityFrames_;
    const std::uint32_t firstFrames = std::min(count, capacityFrames_ - offset);
    const std::size_t firstSamples = static_cast<std::size_t>(firstFrames) * channels_;
    std::memcpy(samples_.data() + static_cast<std::size_t>(offset) * channels_,
                interleaved, firstSamples * sizeof(float));
    const std::uint32_t remainingFrames = count - firstFrames;
    if (remainingFrames != 0) {
        std::memcpy(samples_.data(), interleaved + firstSamples,
                    static_cast<std::size_t>(remainingFrames) * channels_ * sizeof(float));
    }
    writeFrame_.store(writeFrame + count, std::memory_order_release);
    return count;
}

std::uint32_t AudioOutputRing::read(float* interleaved, std::uint32_t frames) noexcept {
    if (interleaved == nullptr || frames == 0) return 0;

    const std::uint32_t readFrame = readFrame_.load(std::memory_order_relaxed);
    const std::uint32_t writeFrame = writeFrame_.load(std::memory_order_acquire);
    const std::uint32_t available = writeFrame - readFrame;
    const std::uint32_t count = std::min(frames, available);
    const std::uint32_t offset = readFrame % capacityFrames_;
    const std::uint32_t firstFrames = std::min(count, capacityFrames_ - offset);
    const std::size_t firstSamples = static_cast<std::size_t>(firstFrames) * channels_;
    std::memcpy(interleaved, samples_.data() + static_cast<std::size_t>(offset) * channels_,
                firstSamples * sizeof(float));
    const std::uint32_t remainingFrames = count - firstFrames;
    if (remainingFrames != 0) {
        std::memcpy(interleaved + firstSamples, samples_.data(),
                    static_cast<std::size_t>(remainingFrames) * channels_ * sizeof(float));
    }
    readFrame_.store(readFrame + count, std::memory_order_release);
    return count;
}

std::uint32_t AudioOutputRing::availableFrames() const noexcept {
    const std::uint32_t writeFrame = writeFrame_.load(std::memory_order_acquire);
    const std::uint32_t readFrame = readFrame_.load(std::memory_order_relaxed);
    return std::min(writeFrame - readFrame, capacityFrames_);
}

std::uint32_t AudioOutputRing::readResampled(float* interleaved, std::uint32_t frames,
                                             double sourceStep) noexcept {
    if (interleaved == nullptr && frames != 0) return 0;
    if (!std::isfinite(sourceStep) || sourceStep < 0.5 || sourceStep > 1.5) sourceStep = 1.0;

    const std::uint32_t readFrame = readFrame_.load(std::memory_order_relaxed);
    const std::uint32_t writeFrame = writeFrame_.load(std::memory_order_acquire);
    const std::uint32_t available = std::min(writeFrame - readFrame, capacityFrames_);
    double phase = readFraction_;
    std::uint32_t produced = 0;
    for (; produced < frames; ++produced) {
        const auto firstOffset = static_cast<std::uint32_t>(phase);
        const double fraction = phase - firstOffset;
        if (firstOffset >= available) break;
        const std::uint32_t secondOffset =
                firstOffset + (fraction > 0.0 && firstOffset + 1 < available ? 1u : 0u);
        const auto firstSlot = (readFrame + firstOffset) % capacityFrames_;
        const auto secondSlot = (readFrame + secondOffset) % capacityFrames_;
        for (std::uint32_t channel = 0; channel < channels_; ++channel) {
            const float first = samples_[static_cast<std::size_t>(firstSlot) * channels_ + channel];
            const float second = samples_[static_cast<std::size_t>(secondSlot) * channels_ + channel];
            interleaved[static_cast<std::size_t>(produced) * channels_ + channel] =
                    first + static_cast<float>(fraction) * (second - first);
        }
        phase += sourceStep;
    }

    const auto consumed = std::min(static_cast<std::uint32_t>(phase), available);
    readFraction_ = phase - std::floor(phase);
    readFrame_.store(readFrame + consumed, std::memory_order_release);
    return produced;
}

void AudioOutputRing::advanceResampled(std::uint32_t frames, double sourceStep) noexcept {
    if (!std::isfinite(sourceStep) || sourceStep < 0.5 || sourceStep > 1.5) sourceStep = 1.0;
    const double phase = readFraction_ + static_cast<double>(frames) * sourceStep;
    const auto consumed = std::min(static_cast<std::uint32_t>(phase), availableFrames());
    readFraction_ = phase - std::floor(phase);
    readFrame_.fetch_add(consumed, std::memory_order_release);
}
