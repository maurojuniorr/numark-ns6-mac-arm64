#include "ControllerActivation.hpp"

#include <array>
#include <cassert>
#include <cstdint>

int main() {
    constexpr std::array<std::uint8_t, 6> replyPart1 = {0xf0, 0x00, 0x01, 0x3f, 0xfd, 0xfd};
    constexpr std::array<std::uint8_t, 3> replyPart2 = {0x00, 0x79, 0x51};
    constexpr std::array<std::uint8_t, 4> replyPart3 = {0xfd, 0x01, 0x02, 0xf7};
    ControllerActivation::HandshakeReplyParser parser;
    assert(!parser.consume(replyPart1.data(), replyPart1.size()));
    assert(parser.consume(replyPart2.data(), replyPart2.size()));
    assert(!parser.consume(replyPart3.data(), replyPart3.size()));

    const auto started = ControllerActivation::Clock::time_point{};
    assert(ControllerActivation::wakeDeadline(started, started + std::chrono::milliseconds(795)) ==
           started + std::chrono::milliseconds(796));
    const auto jogCompleted = started + std::chrono::milliseconds(800);
    assert(ControllerActivation::wakeDeadline(started, jogCompleted) ==
           jogCompleted + std::chrono::milliseconds(1));

    constexpr std::array<std::uint8_t, 4> message = {0xf0, 0x01, 0x02, 0xf7};
    std::array<std::uint8_t, ControllerActivation::kPacketBytes> packet{};

    assert(ControllerActivation::buildPacket(message.data(), message.size(), packet.data()));
    for (std::size_t i = 0; i < message.size(); ++i) assert(packet[i] == message[i]);
    for (std::size_t i = message.size(); i < packet.size() - 1; ++i)
        assert(packet[i] == 0xfd);
    assert(packet.back() == 0);
    assert(!ControllerActivation::buildPacket(nullptr, message.size(), packet.data()));
    assert(!ControllerActivation::buildPacket(message.data(), 0, packet.data()));
    assert(!ControllerActivation::buildPacket(message.data(), packet.size(), packet.data()));
}
