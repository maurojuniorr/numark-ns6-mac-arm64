#include "AudioTransportRuntime.hpp"

#include <new>
#include <chrono>
#include <cstdio>
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

IOReturn AudioTransportRuntime::start(bool midiEnabled, bool inputEnabled) noexcept {
    std::unique_lock<std::mutex> lock(stateMutex_);
    if (worker_.joinable()) return kIOReturnExclusiveAccess;
    stopRequested_.store(false, std::memory_order_release);
    startComplete_ = false;
    startResult_ = kIOReturnNotReady;
    midiEnabled_ = midiEnabled;
    inputEnabled_.store(inputEnabled, std::memory_order_release);
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

void AudioTransportRuntime::setInputEnabled(bool enabled) noexcept {
    CFRunLoopRef runLoop = nullptr;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        inputEnabled_.store(enabled, std::memory_order_release);
        runLoop = workerRunLoop_;
        if (runLoop != nullptr) CFRetain(runLoop);
    }
    if (runLoop != nullptr) {
        CFRunLoopWakeUp(runLoop);
        CFRelease(runLoop);
    }
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
    bool inputRunning = false;
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
            if (inputEnabled_.load(std::memory_order_acquire)) {
                result = input_->start(session_, inputEngine_);
                inputRunning = result == kIOReturnSuccess;
            }
        }
        if (result == kIOReturnSuccess && midiEnabled_) {
            midiInput_ = std::make_unique<MIDIInputReader>();
            const IOReturn midiOutputResult = midiOutput_.start(session_);
            if (midiOutputResult != kIOReturnSuccess) {
                os_log_error(OS_LOG_DEFAULT, "NS6 MIDI output failed to start: 0x%08x",
                             static_cast<unsigned>(midiOutputResult));
            } else if (!midiForwarder_.start()) {
                os_log_error(OS_LOG_DEFAULT, "NS6 MIDI forwarder failed to start");
            } else {
                const IOReturn midiInputResult = midiInput_->start(session_, midiForwarder_);
                if (midiInputResult != kIOReturnSuccess)
                    os_log_error(OS_LOG_DEFAULT, "NS6 MIDI input failed to start: 0x%08x",
                                 static_cast<unsigned>(midiInputResult));
            }
        }
    } catch (const std::bad_alloc&) {
        result = kIOReturnNoMemory;
    }

    if (result != kIOReturnSuccess) {
        CFRunLoopRef runLoop = CFRunLoopGetCurrent();
        midiOutput_.stop();
        IOReturn midiStop = midiInput_ ? midiInput_->stop() : kIOReturnSuccess;
        IOReturn inputStop = input_ ? input_->stop() : kIOReturnSuccess;
        IOReturn outputStop = output_ ? output_->stop() : kIOReturnSuccess;
        IOReturn feedbackStop = feedback_ ? feedback_->stop() : kIOReturnSuccess;
        while (midiStop != kIOReturnSuccess || inputStop != kIOReturnSuccess || outputStop != kIOReturnSuccess ||
               feedbackStop != kIOReturnSuccess) {
            if (runLoop != nullptr) CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.1, true);
            if (inputStop != kIOReturnSuccess && input_ != nullptr)
                inputStop = input_->stop();
            if (midiStop != kIOReturnSuccess && midiInput_ != nullptr)
                midiStop = midiInput_->stop();
            if (outputStop != kIOReturnSuccess && output_ != nullptr)
                outputStop = output_->stop();
            if (feedbackStop != kIOReturnSuccess && feedback_ != nullptr)
                feedbackStop = feedback_->stop();
        }
        output_.reset();
        feedback_.reset();
        input_.reset();
        midiInput_.reset();
        midiForwarder_.stop();
        publishStartResult(result, nullptr);
        return;
    }

    CFRunLoopRef runLoop = CFRunLoopGetCurrent();
    if (runLoop == nullptr) {
        midiOutput_.stop();
        if (midiInput_) (void)midiInput_->stop();
        if (input_) (void)input_->stop();
        if (feedback_) (void)feedback_->stop();
        (void)output_->stop();
        feedback_.reset();
        input_.reset();
        output_.reset();
        midiInput_.reset();
        midiForwarder_.stop();
        publishStartResult(kIOReturnNotReady, nullptr);
        return;
    }
    publishStartResult(kIOReturnSuccess, runLoop);
    auto nextMIDIRecovery = std::chrono::steady_clock::now();
    auto nextMIDIReport = nextMIDIRecovery + std::chrono::seconds(5);
    auto nextOutputRetry = nextMIDIRecovery;
    auto nextOutputReport = nextMIDIRecovery + std::chrono::seconds(5);
    auto nextInputRetry = nextMIDIRecovery;
    auto nextInputReport = nextMIDIRecovery + std::chrono::seconds(5);
    while (!stopRequested_.load(std::memory_order_acquire)) {
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.1, true);
        const std::uint8_t* anomaly = nullptr;
        std::size_t anomalyBytes = 0;
        std::array<float, 2> previous{};
        if (inputEngine_.takeAnomalousBlock(anomaly, anomalyBytes, previous)) {
            if (FILE* file = std::fopen("/private/tmp/ns6-input-anomaly-0259.bin", "wb")) {
                std::fwrite(previous.data(), sizeof(float), previous.size(), file);
                std::fwrite(anomaly, 1, anomalyBytes, file);
                std::fclose(file);
                os_log(OS_LOG_DEFAULT, "NS6 saved anomalous input USB block: %{public}zu bytes", anomalyBytes);
            } else {
                os_log_error(OS_LOG_DEFAULT, "NS6 could not save anomalous input USB block");
            }
        }
        const auto now = std::chrono::steady_clock::now();
        if (output_->stopped() && now >= nextOutputRetry && session_.isCurrentDevice() &&
            !stopRequested_.load(std::memory_order_acquire)) {
            nextOutputRetry = now + std::chrono::seconds(1);
            const IOReturn stopped = output_->stop();
            if (stopped == kIOReturnSuccess) {
                std::unique_ptr<AudioOutWorker> replacement(
                        new (std::nothrow) AudioOutWorker(session_, outputRing_, packetPolicy_));
                if (!replacement) {
                    os_log_error(OS_LOG_DEFAULT, "NS6 OUT recovery allocation failed");
                } else {
                    outputRing_.advanceResampled(outputRing_.availableFrames(), 1.0);
                    output_.swap(replacement);
                    const IOReturn restarted = output_->start();
                    if (restarted == kIOReturnSuccess)
                        os_log(OS_LOG_DEFAULT, "NS6 OUT recovered after USB transfer failure");
                    else
                        os_log_error(OS_LOG_DEFAULT, "NS6 OUT recovery start failed: 0x%08x",
                                     static_cast<unsigned>(restarted));
                }
            } else {
                os_log_error(OS_LOG_DEFAULT, "NS6 OUT recovery drain failed: 0x%08x",
                             static_cast<unsigned>(stopped));
            }
        }
        if (now >= nextOutputReport) {
            const auto out = output_->stats();
            os_log_info(OS_LOG_DEFAULT,
                        "NS6 output health: submitted=%{public}llu completed=%{public}llu failed=%{public}llu non-silent=%{public}llu missing=%{public}llu queue=%{public}u",
                        out.submitted, out.completed, out.failures, out.nonSilentSubmitted,
                        out.missingFrames, outputRing_.availableFrames());
            nextOutputReport = now + std::chrono::seconds(5);
        }
        const bool wantInput = inputEnabled_.load(std::memory_order_acquire);
        if (wantInput && inputRunning && now >= nextInputReport) {
            const auto reader = input_->stats();
            os_log_info(OS_LOG_DEFAULT,
                        "NS6 input health: usb=%{public}llu errors=%{public}llu last=0x%{public}x slow-reads=%{public}llu max-read-us=%{public}llu decoded-nonzero=%{public}llu malformed=%{public}llu unexpected-bytes=%{public}llu sample-jumps=%{public}llu queued=%{public}u dropped=%{public}llu starved=%{public}llu",
                        reader.completedTransfers, reader.transportErrors,
                        static_cast<unsigned>(reader.lastError), reader.readsOver50ms,
                        reader.maxReadMicros, inputEngine_.decodedNonZeroSamples(),
                        inputEngine_.malformedBlocks(), inputEngine_.unexpectedUSBBytes(),
                        inputEngine_.largeSampleJumps(), inputEngine_.ring().availableFrames(),
                        inputEngine_.droppedCaptureFrames(), inputEngine_.starvationFrames());
            nextInputReport = now + std::chrono::seconds(5);
        }
        if (wantInput && !inputRunning && now >= nextInputRetry) {
            const IOReturn inputResult = input_->start(session_, inputEngine_);
            inputRunning = inputResult == kIOReturnSuccess;
            if (!inputRunning) {
                os_log_error(OS_LOG_DEFAULT, "NS6 input start failed: 0x%08x", static_cast<unsigned>(inputResult));
                nextInputRetry = now + std::chrono::seconds(1);
            }
        } else if (!wantInput && inputRunning) {
            const IOReturn inputResult = input_->stop();
            if (inputResult == kIOReturnSuccess) inputRunning = false;
            else os_log_error(OS_LOG_DEFAULT, "NS6 input stop failed: 0x%08x", static_cast<unsigned>(inputResult));
        }
        if (midiEnabled_ && now >= nextMIDIRecovery) {
            nextMIDIRecovery = now + std::chrono::seconds(1);
            if (midiOutput_.stopped()) (void)midiOutput_.start(session_);
            if (midiForwarder_.start()) {
                if (midiInput_->needsRecovery()) (void)midiInput_->stop();
                if (midiInput_->stopped()) {
                    const IOReturn restarted = midiInput_->start(session_, midiForwarder_);
                    if (restarted != kIOReturnSuccess)
                        os_log_error(OS_LOG_DEFAULT, "NS6 MIDI input recovery failed: 0x%08x",
                                     static_cast<unsigned>(restarted));
                }
            }
        }
        if (midiEnabled_ && midiInput_ && now >= nextMIDIReport) {
            const auto midi = midiInput_->stats();
            os_log_info(OS_LOG_DEFAULT,
                        "NS6 MIDI input health: reports=%{public}llu published=%{public}llu errors=%{public}llu last=0x%{public}x stopped=%{public}d recovery=%{public}d",
                        midi.reports, midi.publishedBatches, midi.transferErrors,
                        static_cast<unsigned>(midi.lastError), midiInput_->stopped(),
                        midiInput_->needsRecovery());
            nextMIDIReport = now + std::chrono::seconds(5);
        }
    }

    midiOutput_.stop();
    IOReturn midiResult = midiInput_ ? midiInput_->stop() : kIOReturnSuccess;
    IOReturn inputResult = input_ ? input_->stop() : kIOReturnSuccess;
    IOReturn feedbackResult = feedback_ ? feedback_->stop() : kIOReturnSuccess;
    IOReturn outputResult = output_ ? output_->stop() : kIOReturnSuccess;
    while (midiResult != kIOReturnSuccess || inputResult != kIOReturnSuccess || feedbackResult != kIOReturnSuccess ||
           outputResult != kIOReturnSuccess) {
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.1, true);
        if (inputResult != kIOReturnSuccess && input_ != nullptr)
            inputResult = input_->stop();
        if (midiResult != kIOReturnSuccess && midiInput_ != nullptr)
            midiResult = midiInput_->stop();
        if (feedbackResult != kIOReturnSuccess && feedback_ != nullptr)
            feedbackResult = feedback_->stop();
        if (outputResult != kIOReturnSuccess && output_ != nullptr)
            outputResult = output_->stop();
    }
    feedback_.reset();
    input_.reset();
    output_.reset();
    midiInput_.reset();
    midiForwarder_.stop();
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        if (workerRunLoop_ != nullptr) CFRelease(workerRunLoop_);
        workerRunLoop_ = nullptr;
    }
}

