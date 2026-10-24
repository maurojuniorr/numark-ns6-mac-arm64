#pragma once

#include "../audio-in/AudioInputDecoder.hpp"
#include "../audio-out/AudioOutputRing.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

class AudioInputEngine final {
public:
    AudioInputEngine(std::uint32_t ringCapacityFrames, std::uint32_t maxTransferFrames);

    // Called by the capture worker; the CoreAudio ReadInput path never decodes or blocks.
    std::uint32_t publishUSBBlock(const std::uint8_t* block, std::size_t bytes) noexcept;
    std::uint32_t readInput(float* interleaved, std::uint32_t frames) noexcept;
    AudioOutputRing& ring() noexcept { return ring_; }

    std::uint64_t droppedCaptureFrames() const noexcept {
        return droppedCaptureFrames_.load(std::memory_order_relaxed);
    }
    std::uint64_t starvationFrames() const noexcept {
        return starvationFrames_.load(std::memory_order_relaxed);
    }
    std::uint64_t malformedBlocks() const noexcept {
        return malformedBlocks_.load(std::memory_order_relaxed);
    }
    std::uint64_t decodedNonZeroSamples() const noexcept {
        return decodedNonZeroSamples_.load(std::memory_order_relaxed);
    }

private:
    AudioOutputRing ring_;
    std::vector<float> decoded_;
    const std::uint32_t maxTransferFrames_;
    std::atomic<std::uint64_t> droppedCaptureFrames_{0};
    std::atomic<std::uint64_t> starvationFrames_{0};
    std::atomic<std::uint64_t> malformedBlocks_{0};
    std::atomic<std::uint64_t> decodedNonZeroSamples_{0};
};
