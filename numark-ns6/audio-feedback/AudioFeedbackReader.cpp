#include "AudioFeedbackReader.hpp"

#include <chrono>
#include <cstring>
#include <exception>
#include <os/log.h>

namespace {
constexpr UInt32 kDrainTimeoutMilliseconds = 5000;
}

AudioFeedbackReader::~AudioFeedbackReader() {
    if (running_ || inFlightCount() != 0) std::terminate();
    releaseBuffers();
    if (runLoop_ != nullptr) CFRelease(runLoop_);
}

AudioFeedbackReaderStats AudioFeedbackReader::stats() const noexcept {
    return {packets_.load(std::memory_order_relaxed),
            valid_.load(std::memory_order_relaxed),
            shortPackets_.load(std::memory_order_relaxed),
            unexpectedLengths_.load(std::memory_order_relaxed),
            outOfRange_.load(std::memory_order_relaxed),
            transferErrors_.load(std::memory_order_relaxed),
            skippedIntervals_.load(std::memory_order_relaxed)};
}

IOReturn AudioFeedbackReader::start(NS6DeviceSession& session, AudioFeedbackSink& sink) noexcept {
    auto** interface = session.auxiliaryInterface();
    if (initialized_ || interface == nullptr || session.feedbackPipe() == 0 ||
        session.auxiliaryAsyncEventSource() == nullptr) {
        return kIOReturnNotReady;
    }
    session_ = &session;
    sink_ = &sink;
    runLoop_ = CFRunLoopGetCurrent();
    if (runLoop_ == nullptr) return kIOReturnNotReady;
    CFRetain(runLoop_);

    for (std::uint32_t i = 0; i < kSlotCount; ++i) {
        Slot& slot = slots_[i];
        slot.reader = this;
        slot.index = i;
        IOReturn result = (*interface)->LowLatencyCreateBuffer(
                interface, &slot.data,
                static_cast<IOByteCount>(kPacketCapacity) * kPacketsPerTransfer,
                kUSBLowLatencyReadBuffer);
        if (result != kIOReturnSuccess) {
            releaseBuffers();
            CFRelease(runLoop_);
            runLoop_ = nullptr;
            session_ = nullptr;
            sink_ = nullptr;
            return result;
        }
        void* frameBuffer = nullptr;
        result = (*interface)->LowLatencyCreateBuffer(
                interface, &frameBuffer,
                sizeof(IOUSBLowLatencyIsocFrame) * kPacketsPerTransfer,
                kUSBLowLatencyFrameListBuffer);
        if (result != kIOReturnSuccess) {
            releaseBuffers();
            CFRelease(runLoop_);
            runLoop_ = nullptr;
            session_ = nullptr;
            sink_ = nullptr;
            return result;
        }
        slot.frames = static_cast<IOUSBLowLatencyIsocFrame*>(frameBuffer);
    }

    IOReturn eventResult = session.acquireAuxiliaryEvents(runLoop_);
    if (eventResult != kIOReturnSuccess) {
        releaseBuffers();
        CFRelease(runLoop_);
        runLoop_ = nullptr;
        session_ = nullptr;
        sink_ = nullptr;
        return eventResult;
    }
    sourceAdded_ = true;
    running_ = true;
    initialized_ = true;
    for (auto& slot : slots_) {
        const IOReturn result = submit(slot);
        if (result != kIOReturnSuccess) {
            transferErrors_.fetch_add(1, std::memory_order_relaxed);
            stop();
            return result;
        }
    }
    return kIOReturnSuccess;
}

IOReturn AudioFeedbackReader::stop() noexcept {
    if (!initialized_ || session_ == nullptr) return kIOReturnSuccess;
    running_ = false;
    auto** interface = session_ == nullptr ? nullptr : session_->auxiliaryInterface();
    IOReturn result = kIOReturnSuccess;
    if (inFlightCount() != 0 && interface != nullptr) {
        result = (*interface)->AbortPipe(interface, session_->feedbackPipe());
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(kDrainTimeoutMilliseconds);
        while (inFlightCount() != 0 && std::chrono::steady_clock::now() < deadline) {
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.01, true);
        }
        if (inFlightCount() != 0) return kIOReturnTimeout;
    }
    if (sourceAdded_ && runLoop_ != nullptr) {
        const IOReturn eventResult = session_->releaseAuxiliaryEvents(runLoop_);
        if (result == kIOReturnSuccess) result = eventResult;
        sourceAdded_ = false;
    }
    const IOReturn releaseResult = releaseBuffers();
    if (result == kIOReturnSuccess) result = releaseResult;
    sink_ = nullptr;
    session_ = nullptr;
    return result;
}

IOReturn AudioFeedbackReader::submit(Slot& slot) noexcept {
    if (!running_ || session_ == nullptr || slot.inFlight) return kIOReturnNotReady;
    auto** interface = session_->auxiliaryInterface();
    if (interface == nullptr) return kIOReturnNoDevice;
    for (std::uint32_t i = 0; i < kPacketsPerTransfer; ++i) {
        auto& frame = slot.frames[i];
        frame.frStatus = kIOReturnSuccess;
        frame.frReqCount = kPacketCapacity;
        frame.frActCount = 0;
        frame.frTimeStamp = AbsoluteTime{};
    }
    slot.inFlight = true;
    const IOReturn result = (*interface)->LowLatencyReadIsochPipeAsync(
            interface, session_->feedbackPipe(), slot.data, 0,
            kPacketsPerTransfer, 1,
            slot.frames, completion, &slot);
    if (result != kIOReturnSuccess) slot.inFlight = false;
    return result;
}

void AudioFeedbackReader::completion(void* refcon, IOReturn status, void*) {
    auto& slot = *static_cast<Slot*>(refcon);
    slot.reader->didComplete(slot, status);
}

void AudioFeedbackReader::didComplete(Slot& slot, IOReturn status) noexcept {
    for (std::uint32_t i = 0; i < kPacketsPerTransfer; ++i) {
        const auto& frame = slot.frames[i];
        const std::size_t offset = static_cast<std::size_t>(i) * kPacketCapacity;
        const auto observation = AudioFeedbackParser::parse(
                status == kIOReturnSuccess, frame.frStatus == kIOReturnSuccess,
                static_cast<const std::uint8_t*>(slot.data) + offset, frame.frActCount);
        packets_.fetch_add(1, std::memory_order_relaxed);
        switch (observation.kind) {
            case AudioFeedbackKind::valid:
                valid_.fetch_add(1, std::memory_order_relaxed);
                break;
            case AudioFeedbackKind::shortPacket:
                shortPackets_.fetch_add(1, std::memory_order_relaxed);
                break;
            case AudioFeedbackKind::unexpectedLength:
                unexpectedLengths_.fetch_add(1, std::memory_order_relaxed);
                break;
            case AudioFeedbackKind::outOfRange:
                outOfRange_.fetch_add(1, std::memory_order_relaxed);
                break;
            case AudioFeedbackKind::transferError:
                transferErrors_.fetch_add(1, std::memory_order_relaxed);
                break;
        }
        if (sink_ != nullptr) sink_->publish(observation);
    }
    slot.inFlight = false;
    if (status == kIOReturnNoDevice) {
        running_ = false;
        return;
    }
    if (running_ && submit(slot) != kIOReturnSuccess) {
        transferErrors_.fetch_add(1, std::memory_order_relaxed);
        running_ = false;
    }
}

IOReturn AudioFeedbackReader::releaseBuffers() noexcept {
    if (session_ == nullptr) return kIOReturnNotReady;
    auto** interface = session_->auxiliaryInterface();
    if (interface == nullptr) return kIOReturnNotReady;
    IOReturn result = kIOReturnSuccess;
    for (auto& slot : slots_) {
        if (slot.data != nullptr) {
            const IOReturn released = (*interface)->LowLatencyDestroyBuffer(interface, slot.data);
            if (result == kIOReturnSuccess) result = released;
            slot.data = nullptr;
        }
        if (slot.frames != nullptr) {
            const IOReturn released = (*interface)->LowLatencyDestroyBuffer(interface, slot.frames);
            if (result == kIOReturnSuccess) result = released;
            slot.frames = nullptr;
        }
    }
    return result;
}

std::uint32_t AudioFeedbackReader::inFlightCount() const noexcept {
    std::uint32_t count = 0;
    for (const auto& slot : slots_) count += slot.inFlight ? 1u : 0u;
    return count;
}
