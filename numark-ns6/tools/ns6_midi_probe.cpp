#include "../midi/ControllerActivation.hpp"
#include "../coreaudio/AudioTransportRuntime.hpp"
#include "../coreaudio/AudioOutputEngine.hpp"
#include "../audio-feedback/AudioFeedbackReader.hpp"
#include "../audio-out/AudioOutputRing.hpp"
#include "../audio-out/AudioPacketPolicy.hpp"

#include <CoreFoundation/CoreFoundation.h>
#include <cstdio>
#include <string_view>

namespace {
class NullFeedbackSink final : public AudioFeedbackSink {
public:
    void publish(const AudioFeedbackObservation&) noexcept override {}
};

}

int main(int argc, char** argv) {
    const std::string_view mode = argc == 2 ? argv[1] : "";
    const bool audioContext = mode == "--audio-context";
    const bool configuredContext = mode == "--configured-host";
    const bool outputContext = mode == "--out-context";
    const bool feedbackContext = mode == "--feedback-context";
    const bool fullAudioContext = mode == "--full-audio-context";
    const bool diagnostic = audioContext || configuredContext || outputContext || feedbackContext;
    if (argc > 2 || (argc == 2 && !diagnostic && !fullAudioContext)) {
        std::fprintf(stderr, "usage: ns6_midi_probe [--configured-host|--out-context|--feedback-context|--audio-context|--full-audio-context]\n");
        return 2;
    }
    NS6DeviceSession session;
    IOReturn result = session.open();
    if (result != kIOReturnSuccess) {
        std::fprintf(stderr, "NS6 USB session open failed: 0x%x\n", result);
        return 1;
    }

    auto** interface = session.interface();
    UInt8 number = 0, alternate = 0, endpointCount = 0;
    (*interface)->GetInterfaceNumber(interface, &number);
    (*interface)->GetAlternateSetting(interface, &alternate);
    (*interface)->GetNumEndpoints(interface, &endpointCount);
    std::printf("MIDI interface=%u alternate=%u endpoints=%u IN-pipe=%u OUT-pipe=%u\n",
                number, alternate, endpointCount, session.midiInPipe(), session.midiOutPipe());
    for (UInt8 pipe = 1; pipe <= endpointCount; ++pipe) {
        UInt8 direction = 0, endpoint = 0, type = 0, interval = 0;
        UInt16 packetBytes = 0;
        if ((*interface)->GetPipeProperties(interface, pipe, &direction, &endpoint, &type,
                                            &packetBytes, &interval) == kIOReturnSuccess)
            std::printf("pipe=%u endpoint=0x%02x direction=%u type=%u max-packet=%u interval=%u\n",
                        pipe, (direction == kUSBIn ? 0x80 : 0) | endpoint, direction, type,
                        packetBytes, interval);
    }

    IOReturn outputStopResult = kIOReturnSuccess;
    IOReturn feedbackStopResult = kIOReturnSuccess;
    bool hostReply = false;
    if (fullAudioContext) {
        NullFeedbackSink sink;
        AudioOutputEngine engine(8192);
        AudioTransportRuntime audio(session, sink, engine.ring());
        result = session.configureAudioInputMode();
        if (result == kIOReturnSuccess) result = audio.start();
        if (result == kIOReturnSuccess) {
            outputStopResult = audio.stop();
            if (outputStopResult != kIOReturnSuccess) result = outputStopResult;
        }
        if (result == kIOReturnSuccess)
            result = ControllerActivation::activate(session, &hostReply);
        std::printf("full activation: Host Identify reply 0x51=%s, result=0x%x\n",
                    hostReply ? "observed" : "not observed", result);
        if (result == kIOReturnSuccess) result = session.configureAudioInputMode();
        if (result == kIOReturnSuccess) result = audio.start();
        if (result == kIOReturnSuccess) CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.1, true);
        outputStopResult = audio.stop();
    } else if (diagnostic) {
        result = session.configureAudioInputMode();
        if (result == kIOReturnSuccess) {
            AudioOutputRing ring(8192, 4);
            AudioPacketPolicy policy(44100, 8000);
            AudioOutWorker output(session, ring, policy);
            AudioFeedbackReader feedback;
            NullFeedbackSink sink;
            const bool runOutput = audioContext || outputContext;
            const bool runFeedback = audioContext || feedbackContext;
            if (runOutput) result = output.start();
            if (result == kIOReturnSuccess && runFeedback) {
                feedbackStopResult = feedback.start(session, sink);
            }
            if (result == kIOReturnSuccess && feedbackStopResult == kIOReturnSuccess)
                result = ControllerActivation::probeHostIdentify(session, hostReply);
            if (runOutput && outputStopResult == kIOReturnSuccess)
                outputStopResult = output.stop();
            if (runFeedback && feedbackStopResult == kIOReturnSuccess)
                feedbackStopResult = feedback.stop();
            const auto outputStats = output.stats();
            const auto feedbackStats = feedback.stats();
            std::printf("context: OUT submitted=%llu completed=%llu, feedback packets=%llu, valid=%llu\n",
                        static_cast<unsigned long long>(outputStats.submitted),
                        static_cast<unsigned long long>(outputStats.completed),
                        static_cast<unsigned long long>(feedbackStats.packets),
                        static_cast<unsigned long long>(feedbackStats.valid));
        }
        std::printf("Host Identify reply 0x51=%s\n", hostReply ? "observed" : "not observed");
    } else {
        result = ControllerActivation::activate(session);
    }
    const IOReturn closeResult = session.close();
    std::printf("probe result=0x%x output-stop=0x%x feedback-stop=0x%x session-close=0x%x\n",
                result, outputStopResult, feedbackStopResult, closeResult);
    return result == kIOReturnSuccess && outputStopResult == kIOReturnSuccess &&
                           feedbackStopResult == kIOReturnSuccess && closeResult == kIOReturnSuccess
                   ? 0
                   : 1;
}
