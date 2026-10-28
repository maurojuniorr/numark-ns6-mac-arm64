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
    assert(engine.unexpectedUSBBytes() == 0);
    assert(engine.largeSampleJumps() == 0);
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
    fullBlock[24] = 0x80;
    assert(engine.publishUSBBlock(fullBlock.data(), fullBlock.size()) == 0);
    assert(engine.unexpectedUSBBytes() == 1);

    AudioInputEngine buffered(4096, 64);
    std::array<std::uint8_t, 64 * AudioInputDecoder::kSourceStrideBytes> bufferedBlock{};
    std::array<float, 512 * AudioInputDecoder::kChannelCount> bufferedOutput{};
    assert(buffered.publishUSBBlock(bufferedBlock.data(), bufferedBlock.size()) == 64);
    assert(buffered.readInput(bufferedOutput.data(), 512) == 0);
    assert(buffered.starvationFrames() == 0);
    for (unsigned i = 1; i < 32; ++i)
        assert(buffered.publishUSBBlock(bufferedBlock.data(), bufferedBlock.size()) == 64);
    assert(buffered.readInput(bufferedOutput.data(), 512) == 512);
    assert(buffered.starvationFrames() == 0);

    AudioInputEngine capture(16, 8);
    std::array<std::uint8_t, 8 * AudioInputDecoder::kSourceStrideBytes> burst{};
    for (unsigned frame = 0; frame < 8; ++frame) {
        encodeLane(burst.data() + frame * 64, frame % 2 ? 0x7fffff : 0x800000);
        encodeLane(burst.data() + frame * 64 + 32, frame % 2 ? 0x800000 : 0x7fffff);
    }
    assert(capture.publishUSBBlock(burst.data(), burst.size()) == 8);
    assert(capture.largeSampleJumps() >= 4);
    const std::uint8_t* captured = nullptr;
    std::size_t capturedBytes = 0;
    std::array<float, 2> previous{};
    assert(capture.takeAnomalousBlock(captured, capturedBytes, previous));
    assert(capturedBytes == burst.size() && captured[0] == burst[0]);
    assert(previous[0] == 0.0f && previous[1] == 0.0f);
    assert(!capture.takeAnomalousBlock(captured, capturedBytes, previous));
    std::array<float, 16> samples{};
    assert(capture.readInput(samples.data(), 8) == 8);
    assert(samples[0] == -1.0f && samples[1] > 0.99999f);

}
