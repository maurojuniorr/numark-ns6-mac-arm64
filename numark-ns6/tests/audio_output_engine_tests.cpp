#include "AudioOutputEngine.hpp"

#include <array>
#include <cassert>

int main() {
    static_assert(AudioOutputEngine::kSampleRateHz == 44100);
    static_assert(AudioOutputEngine::kChannelCount == 4);
    static_assert(AudioOutputEngine::kBytesPerFrame == 16);

    AudioOutputEngine engine(4);
    const std::array<float, 20> input{
        0.0f, 0.1f, 0.2f, 0.3f, 1.0f, 1.1f, 1.2f, 1.3f,
        2.0f, 2.1f, 2.2f, 2.3f, 3.0f, 3.1f, 3.2f, 3.3f,
        4.0f, 4.1f, 4.2f, 4.3f};
    assert(engine.writeMix(input.data(), 5) == 4);
    assert(engine.droppedMixFrames() == 1);
    std::array<float, 16> output{};
    assert(engine.ring().read(output.data(), 4) == 4);
    for (std::size_t i = 0; i < output.size(); ++i) assert(output[i] == input[i]);
}
