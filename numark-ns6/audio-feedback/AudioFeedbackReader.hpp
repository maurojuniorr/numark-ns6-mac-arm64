#pragma once

#include "AudioFeedbackParser.hpp"
#include "../usb/NS6DeviceSession.hpp"

#include <CoreFoundation/CoreFoundation.h>
#include <atomic>

class AudioFeedbackSink {
public:
    virtual ~AudioFeedbackSink() = default;
    // Called on the USB completion run loop; implementations must not block.
    virtual void publish(const AudioFeedbackObservation& observation) noexcept = 0;
};

struct AudioFeedbackReaderStats final {
    std::uint64_t packets = 0;
    std::uint64_t valid = 0;
    std::uint64_t shortPackets = 0;
    std::uint64_t unexpectedLengths = 0;
    std::uint64_t outOfRange = 0;
    std::uint64_t transferErrors = 0;
    std::uint64_t skippedIntervals = 0;
};

class AudioFeedbackReader final {
public:
    static constexpr std::uint32_t kSlotCount = 2;
    static constexpr std::uint32_t kPacketsPerTransfer = 4;
    static constexpr std::uint16_t kPacketCapacity = 64;

    AudioFeedbackReader() = default;
    ~AudioFeedbackReader();
    AudioFeedbackReader(const AudioFeedbackReader&) = delete;
    AudioFeedbackReader& operator=(const AudioFeedbackReader&) = delete;

    IOReturn start(NS6DeviceSession& session, AudioFeedbackSink& sink) noexcept;
    // Call on the run loop used by start(); the reader must outlive pending callbacks,
    // including when stop returns a drain timeout.
    IOReturn stop() noexcept;
    AudioFeedbackReaderStats stats() const noexcept;

private:
    struct Slot final {
        AudioFeedbackReader* reader = nullptr;
        std::uint32_t index = 0;
        void* data = nullptr;
        IOUSBLowLatencyIsocFrame* frames = nullptr;
        bool inFlight = false;
    };

    static void completion(void* refcon, IOReturn status, void* argument);
    IOReturn submit(Slot& slot) noexcept;
    void didComplete(Slot& slot, IOReturn status) noexcept;
    IOReturn releaseBuffers() noexcept;
    std::uint32_t inFlightCount() const noexcept;

    NS6DeviceSession* session_ = nullptr;
    AudioFeedbackSink* sink_ = nullptr;
    Slot slots_[kSlotCount]{};
    CFRunLoopRef runLoop_ = nullptr;
    bool running_ = false;
    bool sourceAdded_ = false;
    bool initialized_ = false;
    std::atomic<std::uint64_t> packets_{0};
    std::atomic<std::uint64_t> valid_{0};
    std::atomic<std::uint64_t> shortPackets_{0};
    std::atomic<std::uint64_t> unexpectedLengths_{0};
    std::atomic<std::uint64_t> outOfRange_{0};
    std::atomic<std::uint64_t> transferErrors_{0};
    std::atomic<std::uint64_t> skippedIntervals_{0};
};
