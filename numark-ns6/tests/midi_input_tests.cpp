#include "MIDIInputParser.hpp"
#include "MIDIInputEventEncoder.hpp"

#include <array>
#include <cassert>
#include <cstdint>

int main() {
    MIDIInputParser parser;
    std::array<std::uint8_t, 42> report{};
    report.fill(0xfd);
    std::array<std::uint8_t, MIDIInputParser::kMaximumOutputBytes> output{};

    report[0] = 0xb1;
    report[41] = 0;
    assert(parser.feedReport(report.data(), report.size(), output.data()) == 0);

    report.fill(0xfd);
    report[0] = 0x00;
    report[1] = 0x6e;
    report[2] = 0xb1;
    report[3] = 0x20;
    report[4] = 0x37;
    report[41] = 0;
    assert(parser.feedReport(report.data(), report.size(), output.data()) == 6);
    assert((std::array<std::uint8_t, 6>{output[0], output[1], output[2],
                                        output[3], output[4], output[5]} ==
            std::array<std::uint8_t, 6>{0xb1, 0x00, 0x6e, 0xb1, 0x20, 0x37}));

    report.fill(0xfd);
    report[0] = 0x81;
    report[1] = 0x23;
    report[2] = 0x00;
    report[41] = 0;
    assert(parser.feedReport(report.data(), report.size(), output.data()) == 3);
    assert(output[2] == 0x00);

    report.fill(0xfd);
    report[0] = 0xc1;
    report[1] = 0x05;
    report[41] = 0;
    assert(parser.feedReport(report.data(), report.size(), output.data()) == 2);

    assert(parser.feedReport(nullptr, 42, output.data()) == 0);
    assert(parser.feedReport(report.data(), 0, output.data()) == 0);
    report[41] = 0xfd;
    assert(parser.feedReport(report.data(), report.size(), output.data()) == 0);
    report[41] = 0;
    assert(parser.feedReport(report.data(), report.size() - 1, output.data()) == 0);

    constexpr std::array<std::uint8_t, 5> cc = {0xb1, 0x20, 0x37, 0xc1, 0x05};
    std::array<std::uint32_t, 2> ump{};
    assert(MIDIInputEventEncoder::encode(cc.data(), cc.size(), ump.data(), ump.size()) == 2);
    assert(ump[0] == 0x20b12037);
    assert(ump[1] == 0x20c10500);
    assert(MIDIInputEventEncoder::encode(cc.data(), cc.size(), ump.data(), 1) == 0);
    assert(MIDIInputEventEncoder::encode(cc.data() + 1, cc.size() - 1,
                                         ump.data(), ump.size()) == 0);
}
