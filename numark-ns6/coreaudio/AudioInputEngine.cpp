#include "AudioInputEngine.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

AudioInputEngine::AudioInputEngine(std::uint32_t ringCapacityFrames,
                                   std::uint32_t maxTransferFrames)
        : ring_(ringCapacityFrames, AudioInputDecoder::kChannelCount),
          decoded_(static_cast<std::size_t>(maxTransferFrames) *
                   AudioInputDecoder::kChannelCount),
          maxTransferFrames_(maxTransferFrames),
          targetFrames_(ringCapacityFrames >= 4096 ? 2048u : 0u),
          anomalousBlock_(static_cast<std::size_t>(maxTransferFrames) *
                          AudioInputDecoder::kSourceStrideBytes) {
    if (maxTransferFrames == 0) throw std::invalid_argument("maxTransferFrames must be nonzero");
}

std::uint32_t AudioInputEngine::publishUSBBlock(const std::uint8_t* block,
                                                std::size_t bytes) noexcept {
    const std::size_t frames = AudioInputDecoder::decode(
        block, bytes, decoded_.data(), maxTransferFrames_);
    if (frames == 0) {
        malformedBlocks_.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }

    const std::uint32_t frameCount = static_cast<std::uint32_t>(frames);
    std::uint64_t nonZeroSamples = 0;
    std::uint64_t unexpectedBytes = 0;
    std::uint64_t largeJumps = 0;
    float peak = 0.0f;
    const auto previous = lastDecoded_;
    for (std::size_t i = 0; i < bytes; ++i)
        unexpectedBytes += (block[i] & 0xfeu) != 0;
    unexpectedUSBBytes_.fetch_add(unexpectedBytes, std::memory_order_relaxed);
    for (std::size_t frame = 0; frame < frames; ++frame) {
        const float left = decoded_[frame * 2];
        const float right = decoded_[frame * 2 + 1];
        peak = std::max({peak, std::abs(left), std::abs(right)});
        largeJumps += hasLastDecoded_ &&
                (std::abs(left - lastDecoded_[0]) > 0.5f ||
                 std::abs(right - lastDecoded_[1]) > 0.5f);
        lastDecoded_ = {left, right};
        hasLastDecoded_ = true;
    }
    largeSampleJumps_.fetch_add(largeJumps, std::memory_order_relaxed);
    // Retain one raw anomalous transfer for offline bit-lane analysis.
    if (largeJumps >= 4 && peak > 0.75f) {
        if (!anomalySaved_ && !anomalyCaptured_) {
            std::copy_n(block, bytes, anomalousBlock_.data());
            anomalousBytes_ = bytes;
            anomalousPrevious_ = previous;
            anomalyCaptured_ = true;
        }
    }
    for (std::size_t i = 0; i < frames * AudioInputDecoder::kChannelCount; ++i)
        nonZeroSamples += decoded_[i] != 0.0f;
    decodedNonZeroSamples_.fetch_add(nonZeroSamples, std::memory_order_relaxed);
    const std::uint32_t written = ring_.write(decoded_.data(), frameCount);
    droppedCaptureFrames_.fetch_add(frameCount - written, std::memory_order_relaxed);
    return written;
}

bool AudioInputEngine::takeAnomalousBlock(const std::uint8_t*& data, std::size_t& bytes,
                                           std::array<float, 2>& previous) noexcept {
    if (!anomalyCaptured_) return false;
    data = anomalousBlock_.data();
    bytes = anomalousBytes_;
    previous = anomalousPrevious_;
    anomalyCaptured_ = false;
    anomalySaved_ = true;
    return true;
}


std::uint32_t AudioInputEngine::readInput(float* interleaved,
                                          std::uint32_t frames) noexcept {
    if (interleaved == nullptr || frames == 0) return 0;
    const std::uint32_t queued = ring_.availableFrames();
    if (targetFrames_ != 0 && !primed_) {
        if (queued < targetFrames_) {
            std::fill_n(interleaved, static_cast<std::size_t>(frames) *
                                             AudioInputDecoder::kChannelCount, 0.0f);
            return 0;
        }
        primed_ = true;
    }
    // Track the USB clock without letting short scheduling jitter drain the capture reserve.
    const double step = targetFrames_ == 0 ? 1.0 :
            std::clamp(1.0 + (static_cast<double>(queued) - targetFrames_) * 0.000001,
                       0.998, 1.002);
    const std::uint32_t read = targetFrames_ == 0
            ? ring_.read(interleaved, frames)
            : ring_.readResampled(interleaved, frames, step);
    const std::uint32_t missing = frames - read;
    std::fill_n(interleaved + static_cast<std::size_t>(read) * AudioInputDecoder::kChannelCount,
                static_cast<std::size_t>(missing) * AudioInputDecoder::kChannelCount, 0.0f);
    starvationFrames_.fetch_add(missing, std::memory_order_relaxed);
    return read;
}
