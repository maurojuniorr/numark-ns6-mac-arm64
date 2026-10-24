#pragma once

#include <cstddef>
#include <cstdint>

class AudioInputDecoder final {
public:
    static constexpr std::size_t kSourceStrideBytes = 64;
    static constexpr std::uint32_t kChannelCount = 2;

    // Each stride carries one big-endian 24-bit sample per channel in bit lanes.
    static std::size_t decode(const std::uint8_t* source, std::size_t sourceBytes,
                              float* interleaved, std::size_t outputCapacityFrames) noexcept;
};
