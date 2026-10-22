#pragma once

#include "../usb/NS6DeviceSession.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <chrono>

class ControllerActivation final {
public:
    static constexpr std::size_t kPacketBytes = 42;

    class HandshakeReplyParser final {
    public:
        bool consume(const std::uint8_t* bytes, std::size_t length) noexcept;

    private:
        std::size_t matched_ = 0;
    };

    using Clock = std::chrono::steady_clock;
    using BeforeSessionReopen = IOReturn (*)(void*) noexcept;
    static Clock::time_point wakeDeadline(Clock::time_point started,
                                          Clock::time_point jogCompleted) noexcept;

    static bool buildPacket(const std::uint8_t* message, std::size_t length,
                            std::uint8_t* packet) noexcept;
    // Reopens the USB session before wake; call before audio mode or endpoint workers start.
    static IOReturn activate(NS6DeviceSession& session, bool* replySeen = nullptr,
                             BeforeSessionReopen beforeSessionReopen = nullptr,
                             void* context = nullptr) noexcept;
    // Sends only Host Identify without changing the session, for startup-order diagnostics.
    static IOReturn probeHostIdentify(NS6DeviceSession& session, bool& replySeen) noexcept;
};
