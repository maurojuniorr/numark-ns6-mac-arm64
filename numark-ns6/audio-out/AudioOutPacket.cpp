#include "AudioOutPacket.hpp"

#include <algorithm>
#include <cmath>

std::uint32_t AudioOutPacket::build(AudioOutputRing& ring, AudioPacketPolicy& policy) noexcept {
    bytes_.fill(0);
    byteCount_ = 0;
    missingFrames_ = 0;
    if (ring.channels() != kChannels) return 0;
    const std::uint32_t frames = policy.nextPacketFrames();
    if (frames > kMaxFrames) return 0;

    std::array<float, kMaxFrames * kChannels> samples{};
    const double sourceStep = policy.sourceFramesPerOutputFrame(ring.availableFrames());
    const std::uint32_t available = ring.readResampled(samples.data(), frames, sourceStep);
    missingFrames_ = frames - available;
    if (missingFrames_ != 0) ring.advanceResampled(missingFrames_, sourceStep);
    for (std::uint32_t sample = 0; sample < available * kChannels; ++sample) {
        const float input = std::isfinite(samples[sample]) ? samples[sample] : 0.0f;
        const double scaled = std::clamp(static_cast<double>(input), -1.0, 1.0) * 8388608.0;
        const std::int32_t value = static_cast<std::int32_t>(std::clamp(std::llround(scaled), -8388608LL, 8388607LL));
        const std::uint32_t packed = static_cast<std::uint32_t>(value);
        const std::size_t offset = static_cast<std::size_t>(sample) * kBytesPerSample;
        bytes_[offset] = static_cast<std::uint8_t>(packed);
        bytes_[offset + 1] = static_cast<std::uint8_t>(packed >> 8);
        bytes_[offset + 2] = static_cast<std::uint8_t>(packed >> 16);
    }
    byteCount_ = static_cast<std::size_t>(frames) * kChannels * kBytesPerSample;
    return frames;
}
