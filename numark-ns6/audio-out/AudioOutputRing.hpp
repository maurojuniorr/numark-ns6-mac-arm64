#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

class AudioOutputRing final {
public:
    AudioOutputRing(std::uint32_t capacityFrames, std::uint32_t channels);

    // One producer and one consumer only. Construction allocates; read/write do not.
    std::uint32_t write(const float* interleaved, std::uint32_t frames) noexcept;
    std::uint32_t read(float* interleaved, std::uint32_t frames) noexcept;
    std::uint32_t availableFrames() const noexcept;
    std::uint32_t readResampled(float* interleaved, std::uint32_t frames,
                                double sourceStep) noexcept;
    void advanceResampled(std::uint32_t frames, double sourceStep) noexcept;
    std::uint32_t channels() const noexcept { return channels_; }

private:
    const std::uint32_t capacityFrames_;
    const std::uint32_t channels_;
    std::vector<float> samples_;
    alignas(64) std::atomic<std::uint32_t> writeFrame_{0};
    alignas(64) std::atomic<std::uint32_t> readFrame_{0};
    double readFraction_ = 0.0;
};
