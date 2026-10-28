#include "AudioInputDecoder.hpp"
#include "../audio-in/AudioInputReader.hpp"

#include <array>
#include <cassert>
#include <cstdint>
#include <limits>

static_assert(AudioInputReader::kSlotCount == 8,
              "Diagnostic capture must keep eight bulk-IN reads queued");

static void encodeLane(std::uint8_t* bytes, std::uint32_t sample) {
    for (unsigned bit = 0; bit < 24; ++bit)
        bytes[bit] = static_cast<std::uint8_t>((sample >> (23 - bit)) & 1u);
}

int main() {
    std::array<std::uint8_t, 3 * AudioInputDecoder::kSourceStrideBytes> encoded{};
    std::array<float, 3 * AudioInputDecoder::kChannelCount> decoded{};

    encodeLane(encoded.data(), 0x7fffff);
    encodeLane(encoded.data() + 32, 0x800000);
    encodeLane(encoded.data() + 64, 0x400000);
    encodeLane(encoded.data() + 96, 0xc00000);
    encodeLane(encoded.data() + 128, 0x000001);
    encodeLane(encoded.data() + 160, 0xffffff);

    assert(AudioInputDecoder::decode(encoded.data(), encoded.size(), decoded.data(), 3) == 3);
    assert(decoded[0] > 0.99999f && decoded[0] <= 1.0f);
    assert(decoded[1] == -1.0f);
    assert(decoded[2] == 0.5f);
    assert(decoded[3] == -0.5f);
    assert(decoded[4] > 0.0f && decoded[4] < 0.000001f);
    assert(decoded[5] < 0.0f && decoded[5] > -0.000001f);

    assert(AudioInputDecoder::decode(encoded.data(), encoded.size() - 1, decoded.data(), 3) == 0);
    assert(AudioInputDecoder::decode(encoded.data(), encoded.size(), decoded.data(), 2) == 0);
    assert(AudioInputDecoder::decode(nullptr, encoded.size(), decoded.data(), 3) == 0);
    assert(AudioInputDecoder::decode(encoded.data(), encoded.size(), nullptr, 3) == 0);
}
