#include "AudioTransportRuntime.hpp"

#include <new>
#include <system_error>
#include <pthread.h>
#include <sys/qos.h>
#include <os/log.h>

AudioTransportRuntime::AudioTransportRuntime(NS6DeviceSession& session,
                                             AudioOutputRing& outputRing,
                                             AudioInputEngine& inputEngine) noexcept
        : session_(session), outputRing_(outputRing), inputEngine_(inputEngine) {}

AudioTransportRuntime::~AudioTransportRuntime() {
    (void)stop();
}

void AudioTransportRuntime::publish(const AudioFeedbackObservation& observation) noexcept {
    if (observation.kind == AudioFeedbackKind::valid) {
        packetPolicy_.observeFeedback(observation.framesPerMillisecond);
    } else if (observation.kind != AudioFeedbackKind::transferError) {
        packetPolicy_.observeInvalidFeedback();
    }
}

IOReturn AudioTransportRuntime::start() noexcept {
    std::unique_lock<std::mutex> lock(stateMutex_);
    if (worker_.joinable()) return kIOReturnExclusiveAccess;
    stopRequested_.store(false, std::memory_order_release);
    startComplete_ = false;
    startResult_ = kIOReturnNotReady;
    try {
        worker_ = std::thread(&AudioTransportRuntime::workerMain, this);
    } catch (const std::system_error&) {
        return kIOReturnNoResources;
    }
    stateChanged_.wait(lock, [this] { return startComplete_; });
    const IOReturn result = startResult_;
    lock.unlock();
    if (result != kIOReturnSuccess && worker_.joinable()) worker_.join();
    return result;
}

IOReturn AudioTransportRuntime::stop() noexcept {
    if (!worker_.joinable()) return kIOReturnSuccess;
    CFRunLoopRef runLoop = nullptr;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        stopRequested_.store(true, std::memory_order_release);
        runLoop = workerRunLoop_;
        if (runLoop != nullptr) CFRetain(runLoop);
    }
    if (runLoop != nullptr) {
        CFRunLoopWakeUp(runLoop);
        CFRelease(runLoop);
    }
    worker_.join();
    return kIOReturnSuccess;
}

void AudioTransportRuntime::publishStartResult(IOReturn result,
                                               CFRunLoopRef runLoop) noexcept {
    std::lock_guard<std::mutex> lock(stateMutex_);
    if (runLoop != nullptr) {
        CFRetain(runLoop);
        workerRunLoop_ = runLoop;
    }
    startResult_ = result;
    startComplete_ = true;
    stateChanged_.notify_all();
}

void AudioTransportRuntime::workerMain() noexcept {
    (void)pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    IOReturn result = kIOReturnNoMemory;
    try {
        output_ = std::make_unique<AudioOutWorker>(session_, outputRing_, packetPolicy_);
        result = output_->start();
        if (result == kIOReturnSuccess) {
            feedback_ = std::make_unique<AudioFeedbackReader>();
            const IOReturn feedbackResult = feedback_->start(session_, *this);
            if (feedbackResult != kIOReturnSuccess) {
                os_log_error(OS_LOG_DEFAULT, "NS6 feedback reader failed to start: 0x%08x",
                             static_cast<unsigned>(feedbackResult));
                const IOReturn drainResult = feedback_->stop();
                if (drainResult == kIOReturnSuccess) {
                    feedback_.reset();
                } else {
                    result = drainResult;
                }
            }
        }
        if (result == kIOReturnSuccess) {
            input_ = std::make_unique<AudioInputReader>();
            result = input_->start(session_, inputEngine_);
        }
    } catch (const std::bad_alloc&) {
        result = kIOReturnNoMemory;
    }

    if (result != kIOReturnSuccess) {
        CFRunLoopRef runLoop = CFRunLoopGetCurrent();
        IOReturn inputStop = input_ ? input_->stop() : kIOReturnSuccess;
        IOReturn outputStop = output_ ? output_->stop() : kIOReturnSuccess;
        IOReturn feedbackStop = feedback_ ? feedback_->stop() : kIOReturnSuccess;
        while (inputStop != kIOReturnSuccess || outputStop != kIOReturnSuccess ||
               feedbackStop != kIOReturnSuccess) {
            if (runLoop != nullptr) CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.1, true);
            if (inputStop != kIOReturnSuccess && input_ != nullptr)
                inputStop = input_->stop();
            if (outputStop != kIOReturnSuccess && output_ != nullptr)
                outputStop = output_->stop();
            if (feedbackStop != kIOReturnSuccess && feedback_ != nullptr)
                feedbackStop = feedback_->stop();
        }
        output_.reset();
        feedback_.reset();
        input_.reset();
        publishStartResult(result, nullptr);
        return;
    }

    CFRunLoopRef runLoop = CFRunLoopGetCurrent();
    if (runLoop == nullptr) {
        if (input_) (void)input_->stop();
        if (feedback_) (void)feedback_->stop();
        (void)output_->stop();
        feedback_.reset();
        input_.reset();
        output_.reset();
        publishStartResult(kIOReturnNotReady, nullptr);
        return;
    }
    publishStartResult(kIOReturnSuccess, runLoop);
    while (!stopRequested_.load(std::memory_order_acquire))
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.1, true);

    IOReturn inputResult = input_ ? input_->stop() : kIOReturnSuccess;
    IOReturn feedbackResult = feedback_ ? feedback_->stop() : kIOReturnSuccess;
    IOReturn outputResult = output_ ? output_->stop() : kIOReturnSuccess;
    while (inputResult != kIOReturnSuccess || feedbackResult != kIOReturnSuccess ||
           outputResult != kIOReturnSuccess) {
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.1, true);
        if (inputResult != kIOReturnSuccess && input_ != nullptr)
            inputResult = input_->stop();
        if (feedbackResult != kIOReturnSuccess && feedback_ != nullptr)
            feedbackResult = feedback_->stop();
        if (outputResult != kIOReturnSuccess && output_ != nullptr)
            outputResult = output_->stop();
    }
    feedback_.reset();
    input_.reset();
    output_.reset();
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        if (workerRunLoop_ != nullptr) CFRelease(workerRunLoop_);
        workerRunLoop_ = nullptr;
    }
}

