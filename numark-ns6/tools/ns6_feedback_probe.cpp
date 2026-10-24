#include "../audio-feedback/AudioFeedbackReader.hpp"
#include "../audio-out/AudioOutWorker.hpp"

#include <CoreFoundation/CoreFoundation.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <string_view>

namespace {

class ConsoleSink final : public AudioFeedbackSink {
public:
    void publish(const AudioFeedbackObservation& observation) noexcept override {
        if (observation.kind == AudioFeedbackKind::shortPacket ||
            (observation.kind == AudioFeedbackKind::outOfRange &&
             observation.raw[0] == 0xaa && observation.raw[1] == 0xaa &&
             observation.raw[2] == 0xaa)) {
            return;
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                                     std::chrono::steady_clock::now() - started_)
                                     .count();
        std::printf("+%lld us kind=%u len=%u raw=%02x%02x%02x frames=%u\n",
                    static_cast<long long>(elapsed), static_cast<unsigned>(observation.kind),
                    observation.byteCount, observation.raw[0], observation.raw[1],
                    observation.raw[2], observation.framesPerMillisecond);
        std::fflush(stdout);
    }

    void begin() noexcept { started_ = std::chrono::steady_clock::now(); }

private:
    std::chrono::steady_clock::time_point started_ = std::chrono::steady_clock::now();
};

}  // namespace

int main(int argc, char** argv) {
    const int seconds = argc > 1 ? std::atoi(argv[1]) : 20;
    const bool silentOutput = argc > 2 && std::string_view(argv[2]) == "--silent-output";
    if (seconds < 1 || seconds > 120 || argc > 3 || (argc == 2 && std::string_view(argv[1]) == "--silent-output")) {
        std::fprintf(stderr, "usage: ns6_feedback_probe [1..120 seconds] [--silent-output]\n");
        return 2;
    }

    NS6DeviceSession session;
    IOReturn result = session.open();
    if (result != kIOReturnSuccess) {
        std::fprintf(stderr, "NS6 USB session open failed: 0x%x\n", result);
        return 1;
    }

    UInt64 busFrame = 0;
    UInt64 microframe = 0;
    AbsoluteTime busFrameTime{};
    AbsoluteTime microframeTime{};
    auto** interface = session.interface();
    const IOReturn busFrameResult = (*interface)->GetBusFrameNumber(
            interface, &busFrame, &busFrameTime);
    const IOReturn microframeResult = (*interface)->GetBusMicroFrameNumber(
            interface, &microframe, &microframeTime);
    std::printf("USB frame=%llu (0x%x), microframe=%llu (0x%x)\n",
                static_cast<unsigned long long>(busFrame), busFrameResult,
                static_cast<unsigned long long>(microframe), microframeResult);

    AudioOutputRing ring(8192, 4);
    AudioPacketPolicy packetPolicy(44100, 8000);
    AudioOutWorker output(session, ring, packetPolicy);
    if (silentOutput) {
        result = output.start();
        if (result != kIOReturnSuccess) {
            const auto outputStats = output.stats();
            std::fprintf(stderr, "silent OUT start failed: 0x%x after %llu submissions\n", result,
                         static_cast<unsigned long long>(outputStats.submitted));
            session.close();
            return 1;
        }
    }

    ConsoleSink sink;
    AudioFeedbackReader reader;
    sink.begin();
    result = reader.start(session, sink);
    if (result != kIOReturnSuccess) {
        std::fprintf(stderr, "feedback reader start failed: 0x%x\n", result);
        if (silentOutput) output.stop();
        session.close();
        return 1;
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (std::chrono::steady_clock::now() < deadline) {
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, true);
    }

    result = reader.stop();
    if (result == kIOReturnTimeout) {
        std::fprintf(stderr, "feedback callbacks did not drain; refusing to close the USB session\n");
        std::_Exit(3);
    }
    const IOReturn outputStopResult = silentOutput ? output.stop() : kIOReturnSuccess;
    const auto stats = reader.stats();
    const IOReturn closeResult = session.close();
    std::printf("packets=%llu valid=%llu short=%llu length=%llu range=%llu transfer=%llu skipped=%llu\n",
                static_cast<unsigned long long>(stats.packets),
                static_cast<unsigned long long>(stats.valid),
                static_cast<unsigned long long>(stats.shortPackets),
                static_cast<unsigned long long>(stats.unexpectedLengths),
                static_cast<unsigned long long>(stats.outOfRange),
                static_cast<unsigned long long>(stats.transferErrors),
                static_cast<unsigned long long>(stats.skippedIntervals));
    if (result != kIOReturnSuccess || outputStopResult != kIOReturnSuccess ||
        closeResult != kIOReturnSuccess) {
        std::fprintf(stderr, "shutdown status: reader=0x%x output=0x%x session=0x%x\n",
                     result, outputStopResult, closeResult);
        return 1;
    }
    return 0;
}
