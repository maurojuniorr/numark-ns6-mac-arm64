#pragma once

#include "../coreaudio/AudioInputEngine.hpp"
#include "../usb/NS6DeviceSession.hpp"

#include <CoreFoundation/CoreFoundation.h>
#include <atomic>
#include <cstdint>

struct AudioInputReaderStats final {
    std::uint64_t completedTransfers = 0;
    std::uint64_t transportErrors = 0;
    IOReturn lastError = kIOReturnSuccess;
};

class AudioInputReader final {
public:
    static constexpr std::uint32_t kTransferBytes = 10240;
    static constexpr std::uint32_t kSlotCount = 3;

    AudioInputReader() = default;
    ~AudioInputReader();
    AudioInputReader(const AudioInputReader&) = delete;
    AudioInputReader& operator=(const AudioInputReader&) = delete;

    IOReturn start(NS6DeviceSession& session, AudioInputEngine& engine) noexcept;
    // Stop and drain on the same run loop used by start before destroying the reader/session.
    IOReturn stop() noexcept;
    AudioInputReaderStats stats() const noexcept;

private:
    struct Slot final {
        AudioInputReader* reader = nullptr;
        void* data = nullptr;
        bool inFlight = false;
        bool completed = false;
        IOReturn status = kIOReturnSuccess;
        std::uint32_t bytes = 0;
        std::uint64_t sequence = 0;
    };

    static void completion(void* refcon, IOReturn status, void* argument);
    IOReturn submit(Slot& slot) noexcept;
    void didComplete(Slot& slot, IOReturn status, void* argument) noexcept;
    void drainCompletions() noexcept;
    IOReturn releaseBuffers() noexcept;
    std::uint32_t inFlightCount() const noexcept;

    NS6DeviceSession* session_ = nullptr;
    AudioInputEngine* engine_ = nullptr;
    Slot slots_[kSlotCount]{};
    CFRunLoopRef runLoop_ = nullptr;
    bool running_ = false;
    bool eventsAcquired_ = false;
    std::uint64_t nextSubmitSequence_ = 0;
    std::uint64_t nextProcessSequence_ = 0;
    std::atomic<std::uint64_t> completedTransfers_{0};
    std::atomic<std::uint64_t> transportErrors_{0};
    std::atomic<IOReturn> lastError_{kIOReturnSuccess};
};
