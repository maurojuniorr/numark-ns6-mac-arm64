#include "ControllerActivation.hpp"

#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <os/log.h>
#include <cstdint>
#include <thread>

namespace {
constexpr std::uint8_t kHostIdentify[] = {
        0xf0, 0x00, 0x01, 0x3f, 0x7f, 0x79, 0x50, 0x00, 0x10, 0x04, 0x01, 0x00, 0x00,
        0x00, 0x04, 0x04, 0x0e, 0x0f, 0x00, 0x00, 0x0e, 0x05, 0x0f, 0x04, 0x0c, 0x06,
        0x0b, 0x0f, 0x0d, 0x0c, 0xf7};
constexpr std::uint8_t kJogEnable[] = {
        0xb1, 0x3b, 0x01, 0xb2, 0x3b, 0x01, 0xb3, 0x3b, 0x01, 0xb4, 0x3b, 0x01};
constexpr std::uint8_t kWake[] = {
        0xf0, 0x00, 0x01, 0x3f, 0x7f, 0x79, 0x60, 0x00, 0x01, 0x49, 0x01, 0x00, 0x00, 0x00,
        0x00, 0xf7};
struct InputRead final {
    IOUSBInterfaceInterface** interface = nullptr;
    ControllerActivation::HandshakeReplyParser* handshakeReplyParser = nullptr;
    std::atomic<bool>* handshakeReplySeen = nullptr;
    std::atomic<bool>* firstCompletionLogged = nullptr;
    UInt8 pipe = 0;
    std::array<std::uint8_t, ControllerActivation::kPacketBytes> data{};
    bool running = false;
    bool inFlight = false;
};

IOReturn submit(InputRead& read) noexcept {
    if (!read.running || read.inFlight) return kIOReturnNotReady;
    read.inFlight = true;
    const IOReturn result = (*read.interface)->ReadPipeAsync(
            read.interface, read.pipe, read.data.data(), read.data.size(),
            [](void* refcon, IOReturn status, void* argument) {
                auto& completed = *static_cast<InputRead*>(refcon);
                completed.inFlight = false;
                const auto length = std::min<std::size_t>(
                        reinterpret_cast<std::uintptr_t>(argument), completed.data.size());
                if (status == kIOReturnSuccess && completed.firstCompletionLogged != nullptr &&
                    !completed.firstCompletionLogged->exchange(true, std::memory_order_acq_rel)) {
                    char hex[ControllerActivation::kPacketBytes * 3 + 1]{};
                    static constexpr char digits[] = "0123456789abcdef";
                    const auto count = std::min(length, ControllerActivation::kPacketBytes);
                    for (std::size_t i = 0; i < count; ++i) {
                        hex[i * 3] = digits[completed.data[i] >> 4];
                        hex[i * 3 + 1] = digits[completed.data[i] & 0x0f];
                        if (i + 1 < count) hex[i * 3 + 2] = ' ';
                    }
                    os_log(OS_LOG_DEFAULT,
                           "Numark NS6 MIDI first successful input report: %zu bytes, %{public}s",
                           count, hex);
                }
                if (status == kIOReturnSuccess && completed.handshakeReplySeen != nullptr) {
                    if (completed.handshakeReplyParser->consume(completed.data.data(), length) &&
                        !completed.handshakeReplySeen->exchange(true, std::memory_order_acq_rel))
                        os_log(OS_LOG_DEFAULT, "Numark NS6 MIDI host-identify reply 0x51 received");
                }
                if (status == kIOReturnSuccess && completed.running) (void)submit(completed);
            },
            &read);
    if (result != kIOReturnSuccess) read.inFlight = false;
    return result;
}

IOReturn sendMessage(NS6DeviceSession& session, const std::uint8_t* message,
                     std::size_t length) noexcept {
    auto** interface = session.interface();
    if (interface == nullptr || session.midiOutPipe() == 0) return kIOReturnNotReady;
    std::array<std::uint8_t, ControllerActivation::kPacketBytes> packet{};
    if (!ControllerActivation::buildPacket(message, length, packet.data()))
        return kIOReturnBadArgument;
    const IOReturn result = (*interface)->WritePipeTO(
            interface, session.midiOutPipe(), packet.data(), packet.size(), 1000, 1000);
    if (result != kIOReturnSuccess) {
        const IOReturn pipeStatus = (*interface)->GetPipeStatus(interface, session.midiOutPipe());
        os_log_error(OS_LOG_DEFAULT,
                     "Numark NS6 MIDI OUT write failed: 0x%08x, pipe status 0x%08x",
                     static_cast<unsigned>(result), static_cast<unsigned>(pipeStatus));
    }
    if (result == kIOUSBPipeStalled)
        (*interface)->ClearPipeStallBothEnds(interface, session.midiOutPipe());
    return result;
}

IOReturn sendSequence(NS6DeviceSession& session,
                      const std::atomic<bool>& handshakeReplySeen,
                      ControllerActivation::Clock::time_point& started) noexcept {
    started = ControllerActivation::Clock::now();
    IOReturn firstError = kIOReturnSuccess;
    const auto send = [&](const char* name, const std::uint8_t* message, std::size_t length) {
        const auto writeStarted = ControllerActivation::Clock::now();
        const IOReturn result = sendMessage(session, message, length);
        const auto writeDuration = std::chrono::duration_cast<std::chrono::microseconds>(
                ControllerActivation::Clock::now() - writeStarted).count();
        os_log(OS_LOG_DEFAULT,
               "Numark NS6 MIDI activation %{public}s write result 0x%08x duration %lld us",
               name, static_cast<unsigned>(result), static_cast<long long>(writeDuration));
        if (result != kIOReturnSuccess)
            os_log_error(OS_LOG_DEFAULT, "Numark NS6 MIDI activation %{public}s write failed: 0x%08x",
                         name, static_cast<unsigned>(result));
        if (firstError == kIOReturnSuccess && result != kIOReturnSuccess)
            firstError = result;
    };

    send("host-identify", kHostIdentify, sizeof(kHostIdentify));
    const auto replyDeadline = started + std::chrono::milliseconds(100);
    while (!handshakeReplySeen.load(std::memory_order_acquire) &&
           ControllerActivation::Clock::now() < replyDeadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    os_log(OS_LOG_DEFAULT, "Numark NS6 MIDI activation host-identify reply %{public}s",
           handshakeReplySeen.load(std::memory_order_acquire) ? "observed" : "not observed");
    std::this_thread::sleep_until(started + std::chrono::milliseconds(795));
    send("jog-enable", kJogEnable, sizeof(kJogEnable));
    const auto jogCompleted = ControllerActivation::Clock::now();
    std::this_thread::sleep_until(ControllerActivation::wakeDeadline(started, jogCompleted));
    return firstError;
}

IOReturn stopReads(NS6DeviceSession& session, IOUSBInterfaceInterface** interface,
                   std::array<InputRead, 4>& reads, CFRunLoopRef runLoop) noexcept {
    for (auto& read : reads) read.running = false;
    const IOReturn abortResult = (*interface)->AbortPipe(interface, session.midiInPipe());
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::any_of(reads.begin(), reads.end(), [](const auto& read) { return read.inFlight; }) &&
           std::chrono::steady_clock::now() < deadline)
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.01, true);
    if (std::any_of(reads.begin(), reads.end(), [](const auto& read) { return read.inFlight; }))
        return kIOReturnTimeout;
    const IOReturn eventResult = session.releaseEvents(runLoop);
    if (abortResult != kIOReturnSuccess) return abortResult;
    return eventResult;
}

IOReturn startReads(NS6DeviceSession& session, IOUSBInterfaceInterface** interface,
                    std::array<InputRead, 4>& reads,
                    ControllerActivation::HandshakeReplyParser& handshakeReplyParser,
                    std::atomic<bool>& handshakeReplySeen,
                    std::atomic<bool>& firstCompletionLogged) noexcept {
    for (auto& read : reads) {
        read.interface = interface;
        read.handshakeReplyParser = &handshakeReplyParser;
        read.handshakeReplySeen = &handshakeReplySeen;
        read.firstCompletionLogged = &firstCompletionLogged;
        read.pipe = session.midiInPipe();
        read.running = true;
        const IOReturn result = submit(read);
        if (result != kIOReturnSuccess) {
            for (auto& pending : reads) pending.running = false;
            (*interface)->AbortPipe(interface, session.midiInPipe());
            return result;
        }
    }
    return kIOReturnSuccess;
}
}  // namespace

bool ControllerActivation::HandshakeReplyParser::consume(const std::uint8_t* bytes,
                                                           std::size_t length) noexcept {
    constexpr std::uint8_t prefix[] = {0xf0, 0x00, 0x01, 0x3f, 0x00, 0x79, 0x51};
    if (bytes == nullptr) return false;
    for (std::size_t i = 0; i < length; ++i) {
        const std::uint8_t byte = bytes[i];
        if (byte == 0xfd) continue;
        if (byte == prefix[matched_]) {
            if (++matched_ == sizeof(prefix)) {
                matched_ = 0;
                return true;
            }
        } else {
            matched_ = byte == prefix[0] ? 1 : 0;
        }
    }
    return false;
}

ControllerActivation::Clock::time_point ControllerActivation::wakeDeadline(
        Clock::time_point started, Clock::time_point jogCompleted) noexcept {
    const auto protocolDeadline = started + std::chrono::milliseconds(796);
    const auto postWriteDeadline = jogCompleted + std::chrono::milliseconds(1);
    return std::max(protocolDeadline, postWriteDeadline);
}

bool ControllerActivation::buildPacket(const std::uint8_t* message, std::size_t length,
                                       std::uint8_t* packet) noexcept {
    if (message == nullptr || packet == nullptr || length == 0 || length >= kPacketBytes)
        return false;
    std::memset(packet, 0xfd, kPacketBytes);
    std::memcpy(packet, message, length);
    packet[kPacketBytes - 1] = 0;
    return true;
}

IOReturn ControllerActivation::probeHostIdentify(NS6DeviceSession& session,
                                                 bool& replySeen) noexcept {
    replySeen = false;
    auto** interface = session.interface();
    if (interface == nullptr || session.midiInPipe() == 0 || session.midiOutPipe() == 0)
        return kIOReturnNotReady;
    CFRunLoopRef runLoop = CFRunLoopGetCurrent();
    IOReturn result = session.acquireEvents(runLoop);
    if (result != kIOReturnSuccess) return result;
    result = (*interface)->ClearPipeStallBothEnds(interface, session.midiInPipe());
    if (result == kIOReturnSuccess)
        result = (*interface)->ClearPipeStallBothEnds(interface, session.midiOutPipe());
    if (result != kIOReturnSuccess) {
        session.releaseEvents(runLoop);
        return result;
    }

    std::array<InputRead, 4> reads{};
    HandshakeReplyParser parser;
    std::atomic<bool> reply{false};
    std::atomic<bool> firstReportLogged{false};
    result = startReads(session, interface, reads, parser, reply, firstReportLogged);
    if (result == kIOReturnSuccess) {
        result = sendMessage(session, kHostIdentify, sizeof(kHostIdentify));
        const auto deadline = Clock::now() + std::chrono::milliseconds(100);
        while (result == kIOReturnSuccess && !reply.load(std::memory_order_acquire) &&
               Clock::now() < deadline)
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.005, true);
        replySeen = reply.load(std::memory_order_acquire);
    } else {
        for (auto& read : reads) read.running = false;
        (void)(*interface)->AbortPipe(interface, session.midiInPipe());
    }
    const IOReturn stopResult = stopReads(session, interface, reads, runLoop);
    return result == kIOReturnSuccess ? stopResult : result;
}

IOReturn ControllerActivation::activate(NS6DeviceSession& session, bool* replySeen,
                                        BeforeSessionReopen beforeSessionReopen,
                                        void* context) noexcept {
    if (replySeen != nullptr) *replySeen = false;
    if (session.interface() == nullptr || session.midiInPipe() == 0 ||
        session.midiOutPipe() == 0)
        return kIOReturnNotReady;
    auto** interface = session.interface();
    CFRunLoopRef runLoop = CFRunLoopGetCurrent();
    IOReturn result = session.acquireEvents(runLoop);
    if (result != kIOReturnSuccess) return result;
    result = (*interface)->ClearPipeStallBothEnds(interface, session.midiInPipe());
    if (result != kIOReturnSuccess) {
        session.releaseEvents(runLoop);
        return result;
    }
    result = (*interface)->ClearPipeStallBothEnds(interface, session.midiOutPipe());
    if (result != kIOReturnSuccess) {
        session.releaseEvents(runLoop);
        return result;
    }

    std::array<InputRead, 4> reads{};
    HandshakeReplyParser handshakeReplyParser;
    std::atomic<bool> handshakeReplySeen{false};
    std::atomic<bool> firstCompletionLogged{false};
    result = startReads(session, interface, reads, handshakeReplyParser, handshakeReplySeen,
                        firstCompletionLogged);
    if (result != kIOReturnSuccess) {
        for (auto& read : reads) read.running = false;
        (*interface)->AbortPipe(interface, session.midiInPipe());
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (std::any_of(reads.begin(), reads.end(), [](const auto& read) { return read.inFlight; }) &&
               std::chrono::steady_clock::now() < deadline)
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.01, true);
        if (std::any_of(reads.begin(), reads.end(), [](const auto& read) { return read.inFlight; }))
            return kIOReturnTimeout;
        session.releaseEvents(runLoop);
        return result;
    }

    std::atomic<bool> sequenceComplete{false};
    IOReturn sequenceResult = kIOReturnNotReady;
    Clock::time_point started{};
    try {
        std::thread writer([&] {
            sequenceResult = sendSequence(session, handshakeReplySeen, started);
            sequenceComplete.store(true, std::memory_order_release);
        });
        while (!sequenceComplete.load(std::memory_order_acquire))
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.005, true);
        writer.join();
    } catch (...) {
        (void)stopReads(session, interface, reads, runLoop);
        return kIOReturnNoMemory;
    }
    result = sequenceResult;
    const IOReturn stopResult = stopReads(session, interface, reads, runLoop);
    if (result != kIOReturnSuccess) return result;
    if (stopResult != kIOReturnSuccess) return stopResult;
    if (beforeSessionReopen != nullptr) {
        result = beforeSessionReopen(context);
        if (result != kIOReturnSuccess) return result;
    }
    os_log(OS_LOG_DEFAULT, "Numark NS6 MIDI reopening USB session before wake to reset OUT pipe");
    result = session.close();
    if (result != kIOReturnSuccess) return result;
    result = session.open();
    if (result != kIOReturnSuccess) return result;
    interface = session.interface();
    result = session.acquireEvents(runLoop);
    if (result != kIOReturnSuccess) return result;
    result = (*interface)->ClearPipeStallBothEnds(interface, session.midiInPipe());
    if (result == kIOReturnSuccess)
        result = (*interface)->ClearPipeStallBothEnds(interface, session.midiOutPipe());
    if (result != kIOReturnSuccess) {
        session.releaseEvents(runLoop);
        return result;
    }
    reads = {};
    result = startReads(session, interface, reads, handshakeReplyParser, handshakeReplySeen,
                        firstCompletionLogged);
    if (result != kIOReturnSuccess) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (std::any_of(reads.begin(), reads.end(), [](const auto& read) { return read.inFlight; }) &&
               std::chrono::steady_clock::now() < deadline)
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.01, true);
        session.releaseEvents(runLoop);
        return result;
    }
    std::atomic<bool> wakeComplete{false};
    IOReturn wakeResult = kIOReturnNotReady;
    try {
        std::thread writer([&] {
            wakeResult = sendMessage(session, kWake, sizeof(kWake));
            os_log(OS_LOG_DEFAULT, "Numark NS6 MIDI activation wake write result 0x%08x",
                   static_cast<unsigned>(wakeResult));
            wakeComplete.store(true, std::memory_order_release);
        });
        while (!wakeComplete.load(std::memory_order_acquire))
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.005, true);
        writer.join();
    } catch (...) {
        (void)stopReads(session, interface, reads, runLoop);
        return kIOReturnNoMemory;
    }
    result = wakeResult;
    const bool observedReply = handshakeReplySeen.load(std::memory_order_acquire);
    if (replySeen != nullptr) *replySeen = observedReply;
    if (observedReply) result = kIOReturnSuccess;
    const IOReturn finalStopResult = stopReads(session, interface, reads, runLoop);
    if (result == kIOReturnSuccess && finalStopResult != kIOReturnSuccess)
        return finalStopResult;
    return result;
}
