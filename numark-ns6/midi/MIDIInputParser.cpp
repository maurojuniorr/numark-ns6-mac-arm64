#include "MIDIInputParser.hpp"

std::size_t MIDIInputParser::feedReport(const std::uint8_t* report, std::size_t length,
                                       std::uint8_t* output) noexcept {
    if (report == nullptr || output == nullptr) return 0;
    // Byte 41 is a terminator, not MIDI data; the controller does not keep it at zero.
    if (length != kReportBytes) {
        used_ = 0;
        expected_ = 0;
        return 0;
    }

    std::size_t written = 0;
    for (std::size_t i = 0; i < kReportPayloadBytes; ++i) {
        const std::uint8_t byte = report[i];
        if (byte == 0xfd) continue;

        if (byte & 0x80) {
            used_ = 0;
            expected_ = 0;
            if (byte >= 0x80 && byte <= 0xef) {
                message_[0] = byte;
                used_ = 1;
                expected_ = (byte >= 0xc0 && byte <= 0xdf) ? 2 : 3;
            }
            continue;
        }

        if (expected_ == 0 || used_ >= sizeof(message_)) continue;
        message_[used_++] = byte;
        if (used_ == expected_) {
            for (std::uint8_t j = 0; j < expected_; ++j) output[written++] = message_[j];
            used_ = 0;
            expected_ = 0;
        }
    }
    return written;
}
