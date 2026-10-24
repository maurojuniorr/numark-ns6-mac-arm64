#pragma once

#include "../audio-feedback/AudioFeedbackReader.hpp"
#include "../audio-in/AudioInputReader.hpp"
#include "../audio-out/AudioOutWorker.hpp"
#include "../audio-out/AudioPacketPolicy.hpp"
#include "AudioInputEngine.hpp"
#include "../midi/MIDIInputReader.hpp"
#include "../midi/LocalMIDIForwarder.hpp"
#include "../midi/MIDIOutputWriter.hpp"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <memory>
#include <thread>

class AudioTransportRuntime final : public AudioFeedbackSink {
public:
    AudioTransportRuntime(NS6DeviceSession& session, AudioOutputRing& outputRing,
                          AudioInputEngine& inputEngine) noexcept;
    ~AudioTransportRuntime();
    AudioTransportRuntime(const AudioTransportRuntime&) = delete;
    AudioTransportRuntime& operator=(const AudioTransportRuntime&) = delete;

    IOReturn start(bool midiEnabled = false, bool inputEnabled = false) noexcept;
    IOReturn stop() noexcept;
    void setInputEnabled(bool enabled) noexcept;
    void publish(const AudioFeedbackObservation& observation) noexcept override;
    AudioOutputRing& outputRing() noexcept { return outputRing_; }
private:
    void workerMain() noexcept;
    void publishStartResult(IOReturn result, CFRunLoopRef runLoop) noexcept;

    NS6DeviceSession& session_;
    AudioOutputRing& outputRing_;
    AudioInputEngine& inputEngine_;
    AudioPacketPolicy packetPolicy_{44100, 8000};
    std::unique_ptr<AudioOutWorker> output_;
    std::unique_ptr<AudioFeedbackReader> feedback_;
    std::unique_ptr<AudioInputReader> input_;
    LocalMIDIForwarder midiForwarder_;
    std::unique_ptr<MIDIInputReader> midiInput_;
    MIDIOutputWriter midiOutput_;
    std::thread worker_;
    std::mutex stateMutex_;
    std::condition_variable stateChanged_;
    CFRunLoopRef workerRunLoop_ = nullptr;
    std::atomic<bool> stopRequested_{false};
    std::atomic<bool> inputEnabled_{false};
    bool startComplete_ = false;
    IOReturn startResult_ = kIOReturnNotReady;
    bool midiEnabled_ = false;
};
