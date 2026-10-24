#pragma once

#include "AudioOutputRing.hpp"
#include "AudioPacketPolicy.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

class AudioOutPacket final {
public:
    static constexpr std::uint32_t kMaxFrames = 13;
    static constexpr std::uint32_t kChannels = 4;
    static constexpr std::uint32_t kBytesPerSample = 3;
    static constexpr std::uint32_t kMaxBytes = kMaxFrames * kChannels * kBytesPerSample;

    // Returns zero when the next packet exceeds the endpoint's measured capacity.
    std::uint32_t build(AudioOutputRing& ring, AudioPacketPolicy& policy) noexcept;
    const std::uint8_t* bytes() const noexcept { return bytes_.data(); }
    std::size_t byteCount() const noexcept { return byteCount_; }
    std::uint32_t missingFrames() const noexcept { return missingFrames_; }

private:
    std::array<std::uint8_t, kMaxBytes> bytes_{};
    std::size_t byteCount_ = 0;
    std::uint32_t missingFrames_ = 0;
};
