#include "AudioInputReader.hpp"

#include <chrono>
#include <cstdlib>
#include <exception>
#include <cstdint>
#include <os/log.h>

namespace {
constexpr UInt32 kDrainTimeoutMilliseconds = 5000;
}

AudioInputReader::~AudioInputReader() {
    if (running_ || inFlightCount() != 0) std::terminate();
    releaseBuffers();
    if (runLoop_ != nullptr) CFRelease(runLoop_);
}

AudioInputReaderStats AudioInputReader::stats() const noexcept {
    return {completedTransfers_.load(std::memory_order_relaxed),
            transportErrors_.load(std::memory_order_relaxed),
            lastError_.load(std::memory_order_relaxed),
            readsOver50ms_.load(std::memory_order_relaxed),
            maxReadMicros_.load(std::memory_order_relaxed)};
}

IOReturn AudioInputReader::start(NS6DeviceSession& session,
                                 AudioInputEngine& engine) noexcept {
    if (running_ || inFlightCount() != 0 || session.auxiliaryInterface() == nullptr ||
        session.audioInPipe() == 0 ||
        session.auxiliaryAsyncEventSource() == nullptr) return kIOReturnNotReady;

    session_ = &session;
    engine_ = &engine;
    runLoop_ = CFRunLoopGetCurrent();
    if (runLoop_ == nullptr) {
        session_ = nullptr;
        engine_ = nullptr;
        return kIOReturnNotReady;
    }
    CFRetain(runLoop_);
    nextSubmitSequence_ = 0;
    nextProcessSequence_ = 0;

    for (auto& slot : slots_) {
        slot.reader = this;
        slot.data = std::malloc(kTransferBytes);
        if (slot.data == nullptr) {
            releaseBuffers();
            CFRelease(runLoop_);
            runLoop_ = nullptr;
            session_ = nullptr;
            engine_ = nullptr;
            return kIOReturnNoMemory;
        }
    }

    const IOReturn eventResult = session.acquireAuxiliaryEvents(runLoop_);
    if (eventResult != kIOReturnSuccess) {
        releaseBuffers();
        CFRelease(runLoop_);
        runLoop_ = nullptr;
        session_ = nullptr;
        engine_ = nullptr;
        return eventResult;
    }
    eventsAcquired_ = true;
    const IOReturn clearResult = (*session.auxiliaryInterface())->ClearPipeStallBothEnds(
            session.auxiliaryInterface(), session.audioInPipe());
    if (clearResult != kIOReturnSuccess) {
        session.releaseAuxiliaryEvents(runLoop_);
        eventsAcquired_ = false;
        releaseBuffers();
        CFRelease(runLoop_);
        runLoop_ = nullptr;
        session_ = nullptr;
        engine_ = nullptr;
        return clearResult;
    }
    running_ = true;
    for (auto& slot : slots_) {
        const IOReturn result = submit(slot);
        if (result != kIOReturnSuccess) {
            transportErrors_.fetch_add(1, std::memory_order_relaxed);
            lastError_.store(result, std::memory_order_relaxed);
            stop();
            return result;
        }
    }
    return kIOReturnSuccess;
}

IOReturn AudioInputReader::stop() noexcept {
    if (session_ == nullptr) return kIOReturnSuccess;
    running_ = false;
    auto** interface = session_->auxiliaryInterface();
    IOReturn result = kIOReturnSuccess;
    if (inFlightCount() != 0 && interface != nullptr) {
        result = (*interface)->AbortPipe(interface, session_->audioInPipe());
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(kDrainTimeoutMilliseconds);
        while (inFlightCount() != 0 && std::chrono::steady_clock::now() < deadline)
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.01, true);
        if (inFlightCount() != 0) return kIOReturnTimeout;
    }
    for (auto& slot : slots_) slot.completed = false;
    if (eventsAcquired_) {
        const IOReturn eventResult = session_->releaseAuxiliaryEvents(runLoop_);
        if (result == kIOReturnSuccess) result = eventResult;
        eventsAcquired_ = false;
    }
    const IOReturn releaseResult = releaseBuffers();
    if (result == kIOReturnSuccess) result = releaseResult;
    session_ = nullptr;
    engine_ = nullptr;
    if (runLoop_ != nullptr) {
        CFRelease(runLoop_);
        runLoop_ = nullptr;
    }
    return result;
}

void AudioInputReader::completion(void* refcon, IOReturn status, void* argument) {
    auto& slot = *static_cast<Slot*>(refcon);
    slot.reader->didComplete(slot, status, argument);
}

void AudioInputReader::didComplete(Slot& slot, IOReturn status, void* argument) noexcept {
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - slot.submittedAt).count();
    const auto micros = static_cast<std::uint64_t>(elapsed);
    slot.completedAgeMicros = micros;
    if (micros > 50000) readsOver50ms_.fetch_add(1, std::memory_order_relaxed);
    auto maximum = maxReadMicros_.load(std::memory_order_relaxed);
    while (micros > maximum && !maxReadMicros_.compare_exchange_weak(
            maximum, micros, std::memory_order_relaxed)) {}
    slot.inFlight = false;
    slot.completed = true;
    slot.status = status;
    slot.bytes = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(argument));
    drainCompletions();
}

void AudioInputReader::drainCompletions() noexcept {
    for (;;) {
        Slot* next = nullptr;
        for (auto& slot : slots_) {
            if (slot.completed && slot.sequence == nextProcessSequence_) {
                next = &slot;
                break;
            }
        }
        if (next == nullptr) return;

        const IOReturn completedStatus = next->status;
        if (completedStatus == kIOReturnSuccess) {
            completedTransfers_.fetch_add(1, std::memory_order_relaxed);
            if (engine_ != nullptr) {
                const auto jumps = engine_->largeSampleJumps();
                engine_->publishUSBBlock(static_cast<const std::uint8_t*>(next->data), next->bytes);
                if (engine_->largeSampleJumps() > jumps)
                    os_log(OS_LOG_DEFAULT,
                           "NS6 input anomaly: sequence=%{public}llu read-us=%{public}llu bytes=%{public}u",
                           next->sequence, next->completedAgeMicros, next->bytes);
            }
        } else {
            transportErrors_.fetch_add(1, std::memory_order_relaxed);
            lastError_.store(completedStatus, std::memory_order_relaxed);
            if (completedStatus == kIOReturnNoDevice) running_ = false;
            if (completedStatus == kIOUSBPipeStalled && session_ != nullptr) {
                auto** interface = session_->auxiliaryInterface();
                if (interface == nullptr || (*interface)->ClearPipeStallBothEnds(
                        interface, session_->audioInPipe()) != kIOReturnSuccess)
                    running_ = false;
            }
        }

        next->completed = false;
        ++nextProcessSequence_;
        const IOReturn submitResult = running_ ? submit(*next) : kIOReturnSuccess;
        if (submitResult != kIOReturnSuccess) {
            transportErrors_.fetch_add(1, std::memory_order_relaxed);
            lastError_.store(submitResult, std::memory_order_relaxed);
            running_ = false;
        }
    }
}

IOReturn AudioInputReader::submit(Slot& slot) noexcept {
    if (!running_ || session_ == nullptr || slot.inFlight || slot.completed)
        return kIOReturnNotReady;
    auto** interface = session_->auxiliaryInterface();
    if (interface == nullptr) return kIOReturnNoDevice;

    slot.sequence = nextSubmitSequence_;
    slot.submittedAt = std::chrono::steady_clock::now();
    slot.inFlight = true;
    const IOReturn result = (*interface)->ReadPipeAsync(
            interface, session_->audioInPipe(), slot.data, kTransferBytes, completion, &slot);
    if (result == kIOReturnSuccess) {
        ++nextSubmitSequence_;
    } else {
        slot.inFlight = false;
    }
    return result;
}

IOReturn AudioInputReader::releaseBuffers() noexcept {
    for (auto& slot : slots_) {
        std::free(slot.data);
        slot.data = nullptr;
    }
    return kIOReturnSuccess;
}

std::uint32_t AudioInputReader::inFlightCount() const noexcept {
    std::uint32_t count = 0;
    for (const auto& slot : slots_) count += slot.inFlight ? 1u : 0u;
    return count;
}
