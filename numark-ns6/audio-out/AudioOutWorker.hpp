#pragma once

#include "AudioOutTransferSlots.hpp"
#include "NS6AudioOutLayout.hpp"
#include "../usb/NS6DeviceSession.hpp"

#include <CoreFoundation/CoreFoundation.h>
#include <atomic>

struct AudioOutWorkerStats final {
    std::uint64_t submitted = 0;
    std::uint64_t completed = 0;
    std::uint64_t failures = 0;
    std::uint64_t skippedIntervals = 0;
    std::uint64_t nonSilentSubmitted = 0;
    std::uint64_t missingFrames = 0;
};

class AudioOutWorker final {
public:
    AudioOutWorker(NS6DeviceSession& session, AudioOutputRing& ring,
                   AudioPacketPolicy& packetPolicy) noexcept;
    ~AudioOutWorker();
    AudioOutWorker(const AudioOutWorker&) = delete;
    AudioOutWorker& operator=(const AudioOutWorker&) = delete;

    IOReturn start() noexcept;
    // Call on the run loop used by start(); the worker must outlive pending callbacks,
    // including when stop returns a drain timeout.
    IOReturn stop() noexcept;
    AudioOutWorkerStats stats() const noexcept;
    bool stopped() const noexcept { return !running_; }

private:
    struct Slot final {
        AudioOutWorker* worker = nullptr;
        std::uint32_t index = 0;
        void* data = nullptr;
        IOUSBLowLatencyIsocFrame* frames = nullptr;
    };

    static void completion(void* refcon, IOReturn status, void* argument);
    IOReturn submitNext() noexcept;
    void didComplete(Slot& slot, IOReturn status) noexcept;
    IOReturn releaseBuffers() noexcept;

    NS6DeviceSession& session_;
    AudioOutputRing& ring_;
    AudioPacketPolicy& packetPolicy_;
    AudioOutTransferSlots transferSlots_;
    Slot slots_[AudioOutTransferSlots::kSlotCount]{};
    CFRunLoopRef runLoop_ = nullptr;
    bool running_ = false;
    bool sourceAdded_ = false;
    bool initialized_ = false;
    std::atomic<std::uint64_t> submitted_{0};
    std::atomic<std::uint64_t> completed_{0};
    std::atomic<std::uint64_t> failures_{0};
    std::atomic<std::uint64_t> skippedIntervals_{0};
    std::atomic<std::uint64_t> nonSilentSubmitted_{0};
    std::atomic<std::uint64_t> ringUnderrunFrames_{0};
};
