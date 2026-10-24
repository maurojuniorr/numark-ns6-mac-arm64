#pragma once

#include "../audio-out/AudioOutputRing.hpp"

#include <atomic>
#include <cstdint>

class AudioOutputEngine final {
public:
    static constexpr std::uint32_t kSampleRateHz = 44100;
    static constexpr std::uint32_t kChannelCount = 4;
    static constexpr std::uint32_t kBytesPerFrame = kChannelCount * sizeof(float);

    explicit AudioOutputEngine(std::uint32_t ringCapacityFrames);

    // CoreAudio's WriteMix callback is the single producer; this path never locks or allocates.
    std::uint32_t writeMix(const float* interleaved, std::uint32_t frames) noexcept;
    AudioOutputRing& ring() noexcept { return ring_; }
    std::uint64_t droppedMixFrames() const noexcept {
        return droppedMixFrames_.load(std::memory_order_relaxed);
    }

private:
    AudioOutputRing ring_;
    std::atomic<std::uint64_t> droppedMixFrames_{0};
};
