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
    return socket_ >= 0;
}

void LocalMIDIForwarder::stop() noexcept {
    if (socket_ >= 0) close(socket_);
    socket_ = -1;
    firstSendLogged_ = false;
}

void LocalMIDIForwarder::publish(const std::uint8_t* bytes, std::size_t length) noexcept {
    if (socket_ >= 0 && bytes != nullptr && length > 0 && length <= 128) {
        const sockaddr_in address = bridgeAddress();
        const ssize_t sent = sendto(socket_, bytes, length, 0,
                                    reinterpret_cast<const sockaddr*>(&address), sizeof(address));
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
