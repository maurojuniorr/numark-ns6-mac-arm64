#pragma once

#include "MIDIInputParser.hpp"
#include "../usb/NS6DeviceSession.hpp"

#include <CoreFoundation/CoreFoundation.h>
#include <cstdint>

class MIDIInputSink {
public:
    virtual ~MIDIInputSink() = default;
    // Called in USB completion order on the session's run loop; must not block.
    virtual void publish(const std::uint8_t* bytes, std::size_t length) noexcept = 0;
};

struct MIDIInputReaderStats final {
    std::uint64_t reports = 0;
    std::uint64_t publishedBatches = 0;
    std::uint64_t transferErrors = 0;
    IOReturn lastError = kIOReturnSuccess;
    std::uint64_t nonzeroTerminators = 0;
};

class MIDIInputReader final {
public:
    static constexpr std::uint32_t kSlotCount = 4;

    MIDIInputReader() = default;
    ~MIDIInputReader();
    MIDIInputReader(const MIDIInputReader&) = delete;
    MIDIInputReader& operator=(const MIDIInputReader&) = delete;

    IOReturn start(NS6DeviceSession& session, MIDIInputSink& sink) noexcept;
    IOReturn stop() noexcept;
    bool stopped() const noexcept { return session_ == nullptr; }
    bool needsRecovery() const noexcept { return session_ != nullptr && !running_; }
    MIDIInputReaderStats stats() const noexcept;

private:
    struct Slot final {
        MIDIInputReader* reader = nullptr;
        std::uint8_t data[MIDIInputParser::kReportBytes]{};
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
    std::uint32_t inFlightCount() const noexcept;

    NS6DeviceSession* session_ = nullptr;
    MIDIInputSink* sink_ = nullptr;
    Slot slots_[kSlotCount]{};
    MIDIInputParser parser_{};
    CFRunLoopRef runLoop_ = nullptr;
    bool running_ = false;
    bool eventsAcquired_ = false;
    bool firstMIDIMessageLogged_ = false;
    std::uint64_t nextSubmitSequence_ = 0;
    std::uint64_t nextProcessSequence_ = 0;
    std::uint64_t reports_ = 0;
    std::uint64_t publishedBatches_ = 0;
    std::uint64_t transferErrors_ = 0;
    std::uint64_t nonzeroTerminators_ = 0;
    IOReturn lastError_ = kIOReturnSuccess;
};
