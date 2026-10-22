#include "../audio-in/AudioInputReader.hpp"
#include "../coreaudio/AudioInputEngine.hpp"
#include "../audio-out/AudioOutWorker.hpp"
#include "../midi/ControllerActivation.hpp"

#include <CoreFoundation/CoreFoundation.h>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string_view>

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    const int seconds = argc > 1 ? std::atoi(argv[1]) : 10;
    bool silentOutput = false;
    bool testTone = false;
    bool activateController = false;
    for (int i = 2; i < argc; ++i) {
        const std::string_view option(argv[i]);
        if (option == "--silent-output") silentOutput = true;
        else if (option == "--test-tone") testTone = true;
        else if (option == "--activate-controller") activateController = true;
        else {
            std::fprintf(stderr, "usage: ns6_audio_input_probe [1..120 seconds] [--silent-output|--test-tone] [--activate-controller]\n");
            return 2;
        }
    }
    if (seconds < 1 || seconds > 120 || (silentOutput && testTone)) {
        std::fprintf(stderr, "usage: ns6_audio_input_probe [1..120 seconds] [--silent-output|--test-tone] [--activate-controller]\n");
        return 2;
    }

    NS6DeviceSession session;
    std::puts("stage: open USB session");
    IOReturn result = session.open();
    if (result != kIOReturnSuccess) {
        std::fprintf(stderr, "NS6 USB session open failed: 0x%x\n", result);
        return 1;
    }
    std::puts("stage: USB session open");
    if (activateController) {
        std::puts("stage: MIDI activation begin");
        result = ControllerActivation::activate(session);
        std::printf("stage: MIDI activation result 0x%x\n", result);
        if (result != kIOReturnSuccess) {
            std::fprintf(stderr, "controller activation failed: 0x%x\n", result);
            session.close();
            return 1;
        }
    }
    std::puts("stage: configure input mode");
    result = session.configureAudioInputMode();
    if (result != kIOReturnSuccess) {
        std::fprintf(stderr, "audio input mode setup failed: 0x%x\n", result);
        session.close();
        return 1;
    }
    std::puts("stage: input mode configured");

    AudioOutputRing outputRing(8192, 4);
    AudioPacketPolicy packetPolicy(44100, 8000);
    AudioOutWorker output(session, outputRing, packetPolicy);
    if (silentOutput || testTone) {
        std::puts("stage: start silent audio OUT");
        result = output.start();
        if (result != kIOReturnSuccess) {
            std::fprintf(stderr, "OUT start failed: 0x%x\n", result);
            session.close();
            return 1;
        }
        std::puts("stage: silent audio OUT started");
    }

    AudioInputEngine engine(44100 * 2, AudioInputReader::kTransferBytes / 64);
    AudioInputReader reader;
    std::puts("stage: start audio IN");
    result = reader.start(session, engine);
    if (result != kIOReturnSuccess) {
        std::fprintf(stderr, "audio input start failed: 0x%x\n", result);
        if (silentOutput || testTone) output.stop();
        session.close();
        return 1;
    }
    std::puts("stage: audio IN started");

    std::array<float, 2205 * 2> samples{};
    std::array<float, 2205 * 4> tone{};
    std::uint64_t toneFrame = 0;
    auto intervalStart = std::chrono::steady_clock::now();
    auto deadline = intervalStart + std::chrono::seconds(seconds);
    double squareSum[2] = {0, 0};
    std::uint64_t sampleFrames = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        if (testTone) {
            for (std::size_t frame = 0; frame < tone.size() / 4; ++frame) {
                const float sample = 0.01f * static_cast<float>(
                        std::sin(2.0 * 3.141592653589793 * 220.0 * toneFrame / 44100.0));
                for (std::size_t channel = 0; channel < 4; ++channel)
                    tone[frame * 4 + channel] = sample;
                ++toneFrame;
            }
            outputRing.write(tone.data(), static_cast<std::uint32_t>(tone.size() / 4));
        }
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, true);
        const std::uint32_t frames = engine.readInput(samples.data(), 2205);
        for (std::uint32_t frame = 0; frame < frames; ++frame) {
            for (std::size_t channel = 0; channel < 2; ++channel) {
                const double sample = samples[frame * 2 + channel];
                squareSum[channel] += sample * sample;
            }
        }
        sampleFrames += frames;
        const auto now = std::chrono::steady_clock::now();
        if (now - intervalStart >= std::chrono::seconds(1)) {
            const double denominator = sampleFrames == 0 ? 1.0 : sampleFrames;
            const auto stats = reader.stats();
            const auto outputStats = output.stats();
            std::printf("capture frames=%llu rms=%.6f/%.6f transfers=%llu errors=%llu last=0x%x malformed=%llu dropped=%llu starved=%llu out=%llu/%llu/%llu\n",
                        static_cast<unsigned long long>(sampleFrames),
                        std::sqrt(squareSum[0] / denominator),
                        std::sqrt(squareSum[1] / denominator),
                        static_cast<unsigned long long>(stats.completedTransfers),
                        static_cast<unsigned long long>(stats.transportErrors),
                        stats.lastError,
                        static_cast<unsigned long long>(engine.malformedBlocks()),
                        static_cast<unsigned long long>(engine.droppedCaptureFrames()),
                        static_cast<unsigned long long>(engine.starvationFrames()),
                        static_cast<unsigned long long>(outputStats.completed),
                        static_cast<unsigned long long>(outputStats.submitted),
                        static_cast<unsigned long long>(outputStats.failures));
            std::fflush(stdout);
            intervalStart = now;
            squareSum[0] = squareSum[1] = 0;
            sampleFrames = 0;
        }
    }

    result = reader.stop();
    if (result == kIOReturnTimeout) {
        std::fprintf(stderr, "capture callbacks did not drain; refusing to close the USB session\n");
        std::_Exit(3);
    }
    const IOReturn outputStopResult = silentOutput || testTone ? output.stop() : kIOReturnSuccess;
    const IOReturn closeResult = session.close();
    if (result != kIOReturnSuccess || outputStopResult != kIOReturnSuccess ||
        closeResult != kIOReturnSuccess) {
        std::fprintf(stderr, "shutdown status: input=0x%x output=0x%x session=0x%x\n",
                     result, outputStopResult, closeResult);
        return 1;
    }
    return 0;
}
