#pragma once

#include <cstddef>
#include <cstdint>

class MIDIInputParser final {
public:
    static constexpr std::size_t kReportBytes = 42;
    static constexpr std::size_t kReportPayloadBytes = 41;
    static constexpr std::size_t kMaximumOutputBytes = kReportPayloadBytes + 2;

    // Outputs complete MIDI 1.0 channel messages; state spans report boundaries.
    std::size_t feedReport(const std::uint8_t* report, std::size_t length,
                           std::uint8_t* output) noexcept;

private:
    std::uint8_t message_[3]{};
    std::uint8_t used_ = 0;
    std::uint8_t expected_ = 0;
};
