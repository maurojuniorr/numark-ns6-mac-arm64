#include "LocalMIDIForwarder.hpp"

#include <arpa/inet.h>
#include <os/log.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {
constexpr std::uint16_t kBridgePort = 48239;
const sockaddr_in bridgeAddress() noexcept {
    sockaddr_in address{};
    address.sin_len = sizeof(address);
    address.sin_family = AF_INET;
    address.sin_port = htons(kBridgePort);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return address;
}
}

LocalMIDIForwarder::~LocalMIDIForwarder() { stop(); }

bool LocalMIDIForwarder::start() noexcept {
    if (socket_ >= 0) return true;
    socket_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_ >= 0) {
        const int bufferBytes = 1024 * 1024;
        (void)setsockopt(socket_, SOL_SOCKET, SO_SNDBUF, &bufferBytes, sizeof(bufferBytes));
    }
    return socket_ >= 0;
}

void LocalMIDIForwarder::stop() noexcept {
    if (socket_ >= 0) close(socket_);
    socket_ = -1;
    firstSendLogged_ = false;
}

LocalMIDIForwarderStats LocalMIDIForwarder::stats() const noexcept {
    return {sentDatagrams_, sentBytes_, sendFailures_};
}

void LocalMIDIForwarder::publish(const std::uint8_t* bytes, std::size_t length) noexcept {
    if (socket_ >= 0 && bytes != nullptr && length > 0 && length <= 128) {
        const sockaddr_in address = bridgeAddress();
        const ssize_t sent = sendto(socket_, bytes, length, MSG_DONTWAIT,
                                    reinterpret_cast<const sockaddr*>(&address), sizeof(address));
        if (sent == static_cast<ssize_t>(length)) {
            ++sentDatagrams_;
            sentBytes_ += length;
        } else if (++sendFailures_ == 1 || sendFailures_ % 100 == 0) {
            os_log_error(OS_LOG_DEFAULT, "NS6 MIDI input forwarding failed: %zd/%zu (failures %llu)",
                         sent, length, sendFailures_);
        }
        if (!firstSendLogged_) {
            if (sent == static_cast<ssize_t>(length))
                os_log(OS_LOG_DEFAULT, "Numark NS6 MIDI forwarder first datagram: %zd/%zu bytes",
                       sent, length);
            else
                os_log_error(OS_LOG_DEFAULT,
                             "Numark NS6 MIDI forwarder first datagram failed: %zd/%zu bytes",
                             sent, length);
            firstSendLogged_ = true;
        }
    }
}
