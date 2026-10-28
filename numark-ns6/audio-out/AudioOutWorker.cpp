#include "AudioOutWorker.hpp"

#include <chrono>
#include <cstring>
#include <exception>
#include <os/log.h>

namespace {
constexpr UInt32 kDrainTimeoutMilliseconds = 5000;
}

AudioOutWorker::AudioOutWorker(NS6DeviceSession& session, AudioOutputRing& ring,
                               AudioPacketPolicy& packetPolicy) noexcept
        : session_(session), ring_(ring), packetPolicy_(packetPolicy) {
    for (std::uint32_t i = 0; i < AudioOutTransferSlots::kSlotCount; ++i) {
        slots_[i].worker = this;
        slots_[i].index = i;
    }
}

AudioOutWorker::~AudioOutWorker() {
    if (running_ || transferSlots_.inFlightCount() != 0) std::terminate();
    releaseBuffers();
    if (runLoop_ != nullptr) CFRelease(runLoop_);
}

AudioOutWorkerStats AudioOutWorker::stats() const noexcept {
    return {submitted_.load(std::memory_order_relaxed),
            completed_.load(std::memory_order_relaxed),
            failures_.load(std::memory_order_relaxed),
            skippedIntervals_.load(std::memory_order_relaxed),
            nonSilentSubmitted_.load(std::memory_order_relaxed),
            ringUnderrunFrames_.load(std::memory_order_relaxed)};
}

IOReturn AudioOutWorker::start() noexcept {
    auto** interface = session_.interface();
    if (initialized_ || interface == nullptr || session_.audioOutPipe() == 0 ||
        session_.asyncEventSource() == nullptr) {
        return kIOReturnNotReady;
    }

    runLoop_ = CFRunLoopGetCurrent();
    if (runLoop_ == nullptr) return kIOReturnNotReady;
    CFRetain(runLoop_);

    for (auto& slot : slots_) {
        IOReturn result = (*interface)->LowLatencyCreateBuffer(
                interface, &slot.data, AudioOutTransferBatch::kMaxBytes,
                kUSBLowLatencyWriteBuffer);
        if (result != kIOReturnSuccess) {
            releaseBuffers();
            CFRelease(runLoop_);
            runLoop_ = nullptr;
            return result;
        }
        void* frameBuffer = nullptr;
        result = (*interface)->LowLatencyCreateBuffer(
                interface, &frameBuffer,
                sizeof(IOUSBLowLatencyIsocFrame) * AudioOutTransferBatch::kServiceIntervals,
                kUSBLowLatencyFrameListBuffer);
        if (result != kIOReturnSuccess) {
            releaseBuffers();
            CFRelease(runLoop_);
            runLoop_ = nullptr;
            return result;
        }
        slot.frames = static_cast<IOUSBLowLatencyIsocFrame*>(frameBuffer);
    }

    const IOReturn eventResult = session_.acquireEvents(runLoop_);
    if (eventResult != kIOReturnSuccess) {
        releaseBuffers();
        CFRelease(runLoop_);
        runLoop_ = nullptr;
        return eventResult;
    }
    sourceAdded_ = true;
    running_ = true;
    initialized_ = true;

    for (std::uint32_t i = 0; i < AudioOutTransferSlots::kSlotCount; ++i) {
        const IOReturn result = submitNext();
        if (result != kIOReturnSuccess) {
            failures_.fetch_add(1, std::memory_order_relaxed);
            stop();
            return result;
        }
    }
    return kIOReturnSuccess;
}

IOReturn AudioOutWorker::stop() noexcept {
    if (!initialized_) return kIOReturnSuccess;
    running_ = false;
    transferSlots_.stopAccepting();

    auto** interface = session_.interface();
    IOReturn result = kIOReturnSuccess;
    if (transferSlots_.inFlightCount() != 0 && interface != nullptr) {
        result = (*interface)->AbortPipe(interface, session_.audioOutPipe());
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(kDrainTimeoutMilliseconds);
        while (transferSlots_.inFlightCount() != 0 &&
               std::chrono::steady_clock::now() < deadline) {
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.01, true);
        }
        if (transferSlots_.inFlightCount() != 0) return kIOReturnTimeout;
    }

    if (sourceAdded_ && runLoop_ != nullptr) {
        const IOReturn eventResult = session_.releaseEvents(runLoop_);
        if (result == kIOReturnSuccess) result = eventResult;
        sourceAdded_ = false;
    }
    const IOReturn releaseResult = releaseBuffers();
    if (result == kIOReturnSuccess) result = releaseResult;
    return result;
}

IOReturn AudioOutWorker::submitNext() noexcept {
    auto** interface = session_.interface();
    if (!running_ || interface == nullptr) return kIOReturnNotReady;

    const std::uint32_t slotIndex = transferSlots_.reserve(ring_, packetPolicy_);
    if (slotIndex == AudioOutTransferSlots::kNoSlot) return kIOReturnNoResources;
    Slot& slot = slots_[slotIndex];
    const AudioOutTransferBatch* batch = transferSlots_.packet(slotIndex);
    if (batch == nullptr || batch->byteCount() > AudioOutTransferBatch::kMaxBytes) {
        transferSlots_.complete(slotIndex);
        return kIOReturnBadArgument;
    }
    std::memcpy(slot.data, batch->bytes(), batch->byteCount());
    const auto* pcm = static_cast<const std::uint8_t*>(slot.data);
    for (std::size_t i = 0; i < batch->byteCount(); ++i) {
        if (pcm[i] != 0) {
            nonSilentSubmitted_.fetch_add(1, std::memory_order_relaxed);
            break;
        }
    }
    std::size_t offset = 0;
    for (std::uint32_t i = 0; i < AudioOutTransferBatch::kServiceIntervals; ++i) {
        IOUSBLowLatencyIsocFrame& frame = slot.frames[i];
        frame.frStatus = kIOReturnSuccess;
        frame.frReqCount = batch->packetByteCount(i);
        frame.frActCount = 0;
        frame.frTimeStamp = AbsoluteTime{};
        offset += frame.frReqCount;
    }
    if (offset != batch->byteCount()) {
        transferSlots_.complete(slotIndex);
        return kIOReturnBadArgument;
    }

    const IOReturn result = (*interface)->LowLatencyWriteIsochPipeAsync(
            interface, session_.audioOutPipe(), slot.data, 0,
            AudioOutTransferBatch::kServiceIntervals, 1, slot.frames,
            completion, &slot);
    if (result != kIOReturnSuccess) {
        transferSlots_.complete(slotIndex);
        return result;
    }
    submitted_.fetch_add(1, std::memory_order_relaxed);
    return kIOReturnSuccess;
}

void AudioOutWorker::completion(void* refcon, IOReturn status, void*) {
    auto& slot = *static_cast<Slot*>(refcon);
    slot.worker->didComplete(slot, status);
}

void AudioOutWorker::didComplete(Slot& slot, IOReturn status) noexcept {
    for (std::uint32_t i = 0; i < AudioOutTransferBatch::kServiceIntervals; ++i) {
        if (status == kIOReturnSuccess && slot.frames[i].frStatus != kIOReturnSuccess) {
            status = slot.frames[i].frStatus;
        }
        if (status == kIOReturnSuccess && slot.frames[i].frActCount != slot.frames[i].frReqCount) {
            status = kIOReturnUnderrun;
        }
    }
    transferSlots_.complete(slot.index);
    if (status == kIOReturnSuccess) {
        completed_.fetch_add(1, std::memory_order_relaxed);
        const auto* batch = transferSlots_.packet(slot.index);
        if (batch != nullptr)
            ringUnderrunFrames_.fetch_add(batch->missingFrames(), std::memory_order_relaxed);
    } else {
        failures_.fetch_add(1, std::memory_order_relaxed);
        os_log_error(OS_LOG_DEFAULT,
                     "NS6 OUT stopped: status 0x%08x frame0 status 0x%08x actual/requested %u/%u submitted %{public}llu completed %{public}llu",
                     static_cast<unsigned>(status), static_cast<unsigned>(slot.frames[0].frStatus),
                     slot.frames[0].frActCount, slot.frames[0].frReqCount,
                     static_cast<unsigned long long>(submitted_.load(std::memory_order_relaxed)),
                     static_cast<unsigned long long>(completed_.load(std::memory_order_relaxed)));
        running_ = false;
        transferSlots_.stopAccepting();
        return;
    }
    if (running_ && submitNext() != kIOReturnSuccess) {
        failures_.fetch_add(1, std::memory_order_relaxed);
        os_log_error(OS_LOG_DEFAULT, "NS6 OUT stopped: resubmission failed after %{public}llu completions",
                     static_cast<unsigned long long>(completed_.load(std::memory_order_relaxed)));
        running_ = false;
        transferSlots_.stopAccepting();
    }
}

IOReturn AudioOutWorker::releaseBuffers() noexcept {
    auto** interface = session_.interface();
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
