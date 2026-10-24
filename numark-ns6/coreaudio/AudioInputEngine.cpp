#include "AudioInputEngine.hpp"

#include <algorithm>
#include <stdexcept>

AudioInputEngine::AudioInputEngine(std::uint32_t ringCapacityFrames,
                                   std::uint32_t maxTransferFrames)
        : ring_(ringCapacityFrames, AudioInputDecoder::kChannelCount),
          decoded_(static_cast<std::size_t>(maxTransferFrames) *
                   AudioInputDecoder::kChannelCount),
          maxTransferFrames_(maxTransferFrames) {
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
    for (std::size_t i = 0; i < frames * AudioInputDecoder::kChannelCount; ++i)
        nonZeroSamples += decoded_[i] != 0.0f;
    decodedNonZeroSamples_.fetch_add(nonZeroSamples, std::memory_order_relaxed);
    const std::uint32_t written = ring_.write(decoded_.data(), frameCount);
    droppedCaptureFrames_.fetch_add(frameCount - written, std::memory_order_relaxed);
    return written;
}


std::uint32_t AudioInputEngine::readInput(float* interleaved,
                                          std::uint32_t frames) noexcept {
    if (interleaved == nullptr || frames == 0) return 0;
    const std::uint32_t read = ring_.read(interleaved, frames);
    const std::uint32_t missing = frames - read;
    std::fill_n(interleaved + static_cast<std::size_t>(read) * AudioInputDecoder::kChannelCount,
                static_cast<std::size_t>(missing) * AudioInputDecoder::kChannelCount, 0.0f);
    starvationFrames_.fetch_add(missing, std::memory_order_relaxed);
    return read;
}
