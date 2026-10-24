#include "AudioOutputEngine.hpp"

AudioOutputEngine::AudioOutputEngine(std::uint32_t ringCapacityFrames)
        : ring_(ringCapacityFrames, kChannelCount) {}

std::uint32_t AudioOutputEngine::writeMix(const float* interleaved,
                                          std::uint32_t frames) noexcept {
    const std::uint32_t written = ring_.write(interleaved, frames);
    droppedMixFrames_.fetch_add(frames - written, std::memory_order_relaxed);
    return written;
}
