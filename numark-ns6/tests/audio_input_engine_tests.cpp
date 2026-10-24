#include "AudioInputEngine.hpp"

#include <array>
#include <cassert>
#include <cstdint>

static void encodeLane(std::uint8_t* bytes, std::uint32_t sample) {
    for (unsigned bit = 0; bit < 24; ++bit)
        bytes[bit] = static_cast<std::uint8_t>((sample >> (23 - bit)) & 1u);
}

int main() {
    AudioInputEngine engine(4, 4);
    std::array<std::uint8_t, 2 * AudioInputDecoder::kSourceStrideBytes> block{};
    encodeLane(block.data(), 0x400000);
    encodeLane(block.data() + 32, 0xc00000);
    encodeLane(block.data() + 64, 0x7fffff);
    encodeLane(block.data() + 96, 0x800000);

    assert(engine.publishUSBBlock(block.data(), block.size()) == 2);
    assert(engine.decodedNonZeroSamples() == 4);
    std::array<float, 6> input{};
    assert(engine.readInput(input.data(), 3) == 2);
    assert(input[0] == 0.5f && input[1] == -0.5f);
    assert(input[2] > 0.99999f && input[3] == -1.0f);
    assert(input[4] == 0.0f && input[5] == 0.0f);
    assert(engine.starvationFrames() == 1);

    std::array<std::uint8_t, 4 * AudioInputDecoder::kSourceStrideBytes> fullBlock{};
    assert(engine.publishUSBBlock(fullBlock.data(), fullBlock.size()) == 4);
    assert(engine.publishUSBBlock(block.data(), block.size()) == 0);
    assert(engine.droppedCaptureFrames() == 2);
    assert(engine.publishUSBBlock(block.data(), block.size() - 1) == 0);
    assert(engine.malformedBlocks() == 1);

}
