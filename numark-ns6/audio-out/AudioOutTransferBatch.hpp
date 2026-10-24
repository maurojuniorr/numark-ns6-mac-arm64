#pragma once

#include "AudioOutPacket.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

class AudioOutTransferBatch final {
public:
    static constexpr std::uint32_t kServiceIntervals = 32;
    static constexpr std::size_t kMaxBytes =
            static_cast<std::size_t>(kServiceIntervals) * AudioOutPacket::kMaxBytes;

    bool build(AudioOutputRing& ring, AudioPacketPolicy& policy) noexcept;
    const std::uint8_t* bytes() const noexcept { return bytes_.data(); }
    std::size_t byteCount() const noexcept { return byteCount_; }
    std::uint16_t packetByteCount(std::uint32_t packet) const noexcept;
    std::uint32_t missingFrames() const noexcept { return missingFrames_; }

private:
    std::array<std::uint8_t, kMaxBytes> bytes_{};
    std::array<std::uint16_t, kServiceIntervals> packetByteCounts_{};
    std::size_t byteCount_ = 0;
    std::uint32_t missingFrames_ = 0;
};
