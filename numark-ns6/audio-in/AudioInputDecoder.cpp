#include "AudioInputDecoder.hpp"

namespace {

float decodeLane(const std::uint8_t* lane) noexcept {
    std::uint32_t value = 0;
    for (std::size_t bit = 0; bit < 24; ++bit)
        value = (value << 1) | (lane[bit] & 1u);

    std::int32_t sample = static_cast<std::int32_t>(value);
    if ((value & 0x800000u) != 0) sample -= 0x1000000;
    return static_cast<float>(sample) * (1.0f / 8388608.0f);
}

}  // namespace

std::size_t AudioInputDecoder::decode(const std::uint8_t* source, std::size_t sourceBytes,
                                      float* interleaved,
                                      std::size_t outputCapacityFrames) noexcept {
    if (source == nullptr || interleaved == nullptr || sourceBytes == 0 ||
        sourceBytes % kSourceStrideBytes != 0) return 0;

    const std::size_t frames = sourceBytes / kSourceStrideBytes;
    if (outputCapacityFrames < frames) return 0;

    for (std::size_t frame = 0; frame < frames; ++frame) {
        const std::uint8_t* stride = source + frame * kSourceStrideBytes;
        interleaved[frame * kChannelCount] = decodeLane(stride);
        interleaved[frame * kChannelCount + 1] = decodeLane(stride + 32);
    }
    return frames;
}
