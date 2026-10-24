#include "AudioOutTransferBatch.hpp"

#include <cstring>

bool AudioOutTransferBatch::build(AudioOutputRing& ring, AudioPacketPolicy& policy) noexcept {
    bytes_.fill(0);
    packetByteCounts_.fill(0);
    byteCount_ = 0;
    missingFrames_ = 0;
    AudioOutPacket packet;
    for (std::uint32_t i = 0; i < kServiceIntervals; ++i) {
        const std::uint32_t frames = packet.build(ring, policy);
        if (frames == 0) {
            byteCount_ = 0;
            packetByteCounts_.fill(0);
            return false;
        }
        const auto count = packet.byteCount();
        missingFrames_ += packet.missingFrames();
        std::memcpy(bytes_.data() + byteCount_, packet.bytes(), count);
        packetByteCounts_[i] = static_cast<std::uint16_t>(count);
        byteCount_ += count;
    }
    return true;
}

std::uint16_t AudioOutTransferBatch::packetByteCount(std::uint32_t packet) const noexcept {
    return packet < kServiceIntervals ? packetByteCounts_[packet] : 0;
}
