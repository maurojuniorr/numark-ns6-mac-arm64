#include "MIDIInputReader.hpp"

#include <chrono>
#include <exception>
#include <os/log.h>

namespace {
constexpr auto kDrainTimeout = std::chrono::seconds(5);
}

MIDIInputReader::~MIDIInputReader() {
    if (running_ || inFlightCount() != 0) std::terminate();
}

MIDIInputReaderStats MIDIInputReader::stats() const noexcept {
    return {reports_, publishedBatches_, transferErrors_, lastError_};
}

IOReturn MIDIInputReader::start(NS6DeviceSession& session, MIDIInputSink& sink) noexcept {
    auto** interface = session.interface();
    if (running_ || inFlightCount() != 0 || interface == nullptr || session.midiInPipe() == 0 ||
        session.asyncEventSource() == nullptr) return kIOReturnNotReady;

    session_ = &session;
    sink_ = &sink;
    runLoop_ = CFRunLoopGetCurrent();
    if (runLoop_ == nullptr) {
        session_ = nullptr;
        sink_ = nullptr;
        return kIOReturnNotReady;
    }
    CFRetain(runLoop_);
    parser_ = MIDIInputParser{};
    nextSubmitSequence_ = 0;
    nextProcessSequence_ = 0;
    reports_ = publishedBatches_ = transferErrors_ = 0;
    lastError_ = kIOReturnSuccess;
    firstMIDIMessageLogged_ = false;

    IOReturn result = session.acquireEvents(runLoop_);
    if (result != kIOReturnSuccess) {
        CFRelease(runLoop_);
        runLoop_ = nullptr;
        session_ = nullptr;
        sink_ = nullptr;
        return result;
    }
    eventsAcquired_ = true;
    result = (*interface)->ClearPipeStallBothEnds(interface, session.midiInPipe());
    if (result != kIOReturnSuccess) {
        (void)session.releaseEvents(runLoop_);
        eventsAcquired_ = false;
        CFRelease(runLoop_);
        runLoop_ = nullptr;
        session_ = nullptr;
        sink_ = nullptr;
        return result;
    }

    running_ = true;
    for (auto& slot : slots_) {
        slot.reader = this;
        result = submit(slot);
        if (result != kIOReturnSuccess) {
            ++transferErrors_;
            lastError_ = result;
            (void)stop();
            return result;
        }
    }
    return kIOReturnSuccess;
}

IOReturn MIDIInputReader::stop() noexcept {
    if (session_ == nullptr) return kIOReturnSuccess;
    running_ = false;
    auto** interface = session_->interface();
    IOReturn result = kIOReturnSuccess;
    if (inFlightCount() != 0 && interface != nullptr) {
        result = (*interface)->AbortPipe(interface, session_->midiInPipe());
        const auto deadline = std::chrono::steady_clock::now() + kDrainTimeout;
        while (inFlightCount() != 0 && std::chrono::steady_clock::now() < deadline)
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.01, true);
        if (inFlightCount() != 0) return kIOReturnTimeout;
    }
    for (auto& slot : slots_) slot.completed = false;
    if (eventsAcquired_) {
        const IOReturn releaseResult = session_->releaseEvents(runLoop_);
        if (result == kIOReturnSuccess) result = releaseResult;
        eventsAcquired_ = false;
    }
    sink_ = nullptr;
    session_ = nullptr;
    if (runLoop_ != nullptr) {
        CFRelease(runLoop_);
        runLoop_ = nullptr;
    }
    return result;
}

void MIDIInputReader::completion(void* refcon, IOReturn status, void* argument) {
    auto& slot = *static_cast<Slot*>(refcon);
    slot.reader->didComplete(slot, status, argument);
}

void MIDIInputReader::didComplete(Slot& slot, IOReturn status, void* argument) noexcept {
    slot.inFlight = false;
    slot.completed = true;
    slot.status = status;
    slot.bytes = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(argument));
    drainCompletions();
}

void MIDIInputReader::drainCompletions() noexcept {
    for (;;) {
        Slot* next = nullptr;
        for (auto& slot : slots_) {
            if (slot.completed && slot.sequence == nextProcessSequence_) {
                next = &slot;
                break;
            }
        }
        if (next == nullptr) return;

        if (next->status == kIOReturnSuccess && next->bytes == MIDIInputParser::kReportBytes) {
            ++reports_;
            std::uint8_t messages[MIDIInputParser::kMaximumOutputBytes]{};
            const std::size_t length = parser_.feedReport(next->data, next->bytes, messages);
            if (length != 0 && sink_ != nullptr) {
                ++publishedBatches_;
                if (!firstMIDIMessageLogged_) {
                    char hex[49]{};
                    static constexpr char digits[] = "0123456789abcdef";
                    const std::size_t count = length < 16 ? length : 16;
                    for (std::size_t i = 0; i < count; ++i) {
                        hex[i * 3] = digits[messages[i] >> 4];
                        hex[i * 3 + 1] = digits[messages[i] & 0x0f];
                        if (i + 1 < count) hex[i * 3 + 2] = ' ';
                    }
                    os_log(OS_LOG_DEFAULT,
                           "Numark NS6 MIDI reader first published message: %zu bytes, %{public}s",
                           count, hex);
                    firstMIDIMessageLogged_ = true;
                }
                sink_->publish(messages, length);
            }
        } else {
            ++transferErrors_;
            lastError_ = next->status == kIOReturnSuccess ? kIOReturnUnderrun : next->status;
            running_ = false;
        }

        next->completed = false;
        ++nextProcessSequence_;
        if (running_) {
            const IOReturn result = submit(*next);
            if (result != kIOReturnSuccess) {
                ++transferErrors_;
                lastError_ = result;
                running_ = false;
            }
        }
    }
}

IOReturn MIDIInputReader::submit(Slot& slot) noexcept {
    if (!running_ || session_ == nullptr || slot.inFlight || slot.completed)
        return kIOReturnNotReady;
    auto** interface = session_->interface();
    if (interface == nullptr) return kIOReturnNoDevice;

    slot.sequence = nextSubmitSequence_;
    slot.inFlight = true;
    const IOReturn result = (*interface)->ReadPipeAsync(
            interface, session_->midiInPipe(), slot.data, sizeof(slot.data), completion, &slot);
    if (result == kIOReturnSuccess) ++nextSubmitSequence_;
    else slot.inFlight = false;
    return result;
}

std::uint32_t MIDIInputReader::inFlightCount() const noexcept {
    std::uint32_t count = 0;
    for (const auto& slot : slots_) count += slot.inFlight ? 1u : 0u;
    return count;
}
