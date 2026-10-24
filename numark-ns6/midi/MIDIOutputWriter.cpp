#include "MIDIOutputWriter.hpp"
#include "ControllerActivation.hpp"

#include <arpa/inet.h>
#include <os/log.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <array>
#include <cerrno>
#include <chrono>
#include <system_error>

namespace { constexpr std::uint16_t kDriverPort = 48238; }

MIDIOutputWriter::~MIDIOutputWriter() { stop(); }

IOReturn MIDIOutputWriter::start(NS6DeviceSession& session) noexcept {
    if (worker_.joinable() || session.interface() == nullptr || session.midiOutPipe() == 0)
        return kIOReturnNotReady;
    socket_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_ < 0) return kIOReturnNoResources;
    const int bufferBytes = 1024 * 1024;
    (void)setsockopt(socket_, SOL_SOCKET, SO_RCVBUF, &bufferBytes, sizeof(bufferBytes));
    sockaddr_in address{};
    address.sin_len = sizeof(address);
    address.sin_family = AF_INET;
    address.sin_port = htons(kDriverPort);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(socket_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        close(socket_);
        socket_ = -1;
        return kIOReturnExclusiveAccess;
    }
    session_ = &session;
    running_.store(true);
    try {
        worker_ = std::thread(&MIDIOutputWriter::run, this);
    } catch (const std::system_error&) {
        running_.store(false);
        session_ = nullptr;
        close(socket_);
        socket_ = -1;
        return kIOReturnNoResources;
    }
    return kIOReturnSuccess;
}

void MIDIOutputWriter::stop() noexcept {
    running_.store(false);
    if (worker_.joinable()) worker_.join();
    if (socket_ >= 0) close(socket_);
    socket_ = -1;
    session_ = nullptr;
}

void MIDIOutputWriter::run() noexcept {
    std::uint64_t sent = 0, failures = 0;
    while (running_.load()) {
        pollfd descriptor{socket_, POLLIN, 0};
        const int ready = poll(&descriptor, 1, 50);
        if (ready <= 0 || (descriptor.revents & POLLIN) == 0) continue;
        std::uint8_t message[128]{};
        const ssize_t length = recv(socket_, message, sizeof(message), 0);
        if (length <= 0 || length > 41) { ++failures; continue; }
        std::array<std::uint8_t, ControllerActivation::kPacketBytes> packet{};
        if (!ControllerActivation::buildPacket(message, static_cast<std::size_t>(length), packet.data())) {
            ++failures;
            continue;
        }
        auto** interface = session_->interface();
        if (interface == nullptr) { ++failures; continue; }
        IOReturn result = kIOReturnNotReady;
        for (unsigned attempt = 0; attempt < 3; ++attempt) {
            result = (*interface)->WritePipeTO(interface, session_->midiOutPipe(),
                                               packet.data(), packet.size(), 100, 100);
            if (result == kIOReturnSuccess) break;
            if (result != kIOUSBPipeStalled && result != kIOReturnNotResponding &&
                result != kIOReturnTimeout) break;
            if ((*interface)->ClearPipeStallBothEnds(interface, session_->midiOutPipe()) != kIOReturnSuccess)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (result == kIOReturnSuccess) {
            if (++sent == 1) os_log(OS_LOG_DEFAULT, "NS6 MIDI OUT first USB report sent");
        }
        else {
            ++failures;
            os_log_error(OS_LOG_DEFAULT, "NS6 MIDI OUT write failed: 0x%08x (sent %llu, failures %llu)",
                         static_cast<unsigned>(result), sent, failures);
        }
    }
    os_log(OS_LOG_DEFAULT, "NS6 MIDI OUT stopped: %llu reports sent, %llu failures", sent, failures);
}
