#include "../audio-feedback/AudioFeedbackParser.hpp"
#include "../audio-feedback/AudioFeedbackPatternPolicy.hpp"
#include "../audio-feedback/AudioFeedbackSchedule.hpp"

#include <cassert>
#include <cstdint>

namespace {

void validTripletUsesItsFirstByteAsFramesPerMillisecond() {
    const std::uint8_t report[] = {0x2c, 0x2c, 0x2c};
    const auto sample = AudioFeedbackParser::parse(true, true, report, sizeof(report));
    assert(sample.kind == AudioFeedbackKind::valid);
    assert(sample.framesPerMillisecond == 44);
    assert(sample.raw[0] == 0x2c && sample.raw[1] == 0x2c && sample.raw[2] == 0x2c);
}

void acceptsOnlyObservedFeedbackRange() {
    for (std::uint8_t frames = 42; frames <= 46; ++frames) {
        const std::uint8_t report[] = {frames, 0x2c, 0x2c};
        const auto sample = AudioFeedbackParser::parse(true, true, report, sizeof(report));
        assert(sample.kind == AudioFeedbackKind::valid);
        assert(sample.framesPerMillisecond == frames);
    }
}

void fillerIsPreservedButNeverTreatedAsClockFeedback() {
    const std::uint8_t report[] = {0xaa, 0xaa, 0xaa};
    const auto sample = AudioFeedbackParser::parse(true, true, report, sizeof(report));
    assert(sample.kind == AudioFeedbackKind::outOfRange);
    assert(sample.raw[0] == 0xaa && sample.raw[1] == 0xaa && sample.raw[2] == 0xaa);
}

void shortAndUnexpectedReportsAreRejected() {
    const std::uint8_t shortReport[] = {0x2c, 0x2c};
    const auto shortSample = AudioFeedbackParser::parse(true, true, shortReport,
                                                        sizeof(shortReport));
    assert(shortSample.kind == AudioFeedbackKind::shortPacket);
    assert(shortSample.byteCount == 2);

    const std::uint8_t longReport[] = {0x2c, 0x2c, 0x2c, 0x00};
    const auto longSample = AudioFeedbackParser::parse(true, true, longReport,
                                                       sizeof(longReport));
    assert(longSample.kind == AudioFeedbackKind::unexpectedLength);
    assert(longSample.byteCount == 4);
}

void transferAndFrameErrorsCannotPublishFeedback() {
    const std::uint8_t report[] = {0x2c, 0x2c, 0x2c};
    assert(AudioFeedbackParser::parse(false, true, report, sizeof(report)).kind ==
           AudioFeedbackKind::transferError);
    assert(AudioFeedbackParser::parse(true, false, report, sizeof(report)).kind ==
           AudioFeedbackKind::transferError);
}

void feedbackScheduleMaintainsFourMillisecondCadenceAndSkipsLateIntervals() {
    AudioFeedbackSchedule schedule;
    assert(schedule.begin(100) == 102);
    assert(schedule.takeNext(100).startFrame == 106);
    assert(schedule.takeNext(102).startFrame == 110);

    AudioFeedbackSchedule delayed;
    delayed.begin(100);
    const auto next = delayed.takeNext(115);
    assert(next.startFrame == 118);
    assert(next.skippedIntervals == 3);
}

void feedbackPatternDistributesEightWholePacketsToTheRequestedTotal() {
    const std::array<std::array<std::uint8_t, 8>, 5> expected{{
        {{6, 5, 5, 5, 6, 5, 5, 5}},
        {{5, 5, 6, 5, 6, 5, 6, 5}},
        {{6, 5, 6, 5, 6, 5, 6, 5}},
        {{6, 6, 5, 6, 5, 6, 5, 6}},
        {{5, 6, 6, 6, 5, 6, 6, 6}},
    }};
    for (std::uint8_t target = 42; target <= 46; ++target) {
        AudioFeedbackPatternPolicy policy;
        std::array<std::uint8_t, AudioFeedbackPatternPolicy::kPacketsPerMillisecond> packets{};
        assert(policy.build(target, packets));
        assert(packets == expected[target - 42]);
        std::uint32_t total = 0;
        for (const auto frames : packets) {
            assert(frames == 5 || frames == 6);
            total += frames;
        }
        assert(total == target);
    }

    AudioFeedbackPatternPolicy policy;
    std::array<std::uint8_t, AudioFeedbackPatternPolicy::kPacketsPerMillisecond> nominal{};
    assert(policy.build(44, nominal));
    assert((nominal == std::array<std::uint8_t, 8>{6, 5, 6, 5, 6, 5, 6, 5}));
}

void invalidFeedbackDoesNotOverwriteTheFallbackPattern() {
    AudioFeedbackPatternPolicy policy;
    std::array<std::uint8_t, AudioFeedbackPatternPolicy::kPacketsPerMillisecond> packets{};
    packets.fill(5);
    assert(!policy.build(0xaa, packets));
    for (const auto frames : packets) assert(frames == 5);
}

}  // namespace

int main() {
    validTripletUsesItsFirstByteAsFramesPerMillisecond();
    acceptsOnlyObservedFeedbackRange();
    fillerIsPreservedButNeverTreatedAsClockFeedback();
    shortAndUnexpectedReportsAreRejected();
    transferAndFrameErrorsCannotPublishFeedback();
    feedbackScheduleMaintainsFourMillisecondCadenceAndSkipsLateIntervals();
    feedbackPatternDistributesEightWholePacketsToTheRequestedTotal();
    invalidFeedbackDoesNotOverwriteTheFallbackPattern();
}
