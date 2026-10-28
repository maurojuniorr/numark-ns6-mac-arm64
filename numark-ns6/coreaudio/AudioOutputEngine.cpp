#include "AudioOutputEngine.hpp"

#include <mach/mach_time.h>

AudioOutputEngine::AudioOutputEngine(std::uint32_t ringCapacityFrames)
        : ring_(ringCapacityFrames, kChannelCount) {}

std::uint32_t AudioOutputEngine::writeMix(const float* interleaved,
                                          std::uint32_t frames) noexcept {
    const auto tick = mach_absolute_time();
    const auto previous = lastMixTick_.exchange(tick, std::memory_order_relaxed);
    if (previous != 0) {
        const auto gap = tick - previous;
        auto maximum = maxMixGapTicks_.load(std::memory_order_relaxed);
        while (gap > maximum && !maxMixGapTicks_.compare_exchange_weak(
                maximum, gap, std::memory_order_relaxed)) {}
    }
    mixCalls_.fetch_add(1, std::memory_order_relaxed);
    mixFrames_.fetch_add(frames, std::memory_order_relaxed);
    const std::uint32_t written = ring_.write(interleaved, frames);
    droppedMixFrames_.fetch_add(frames - written, std::memory_order_relaxed);
    return written;
}
