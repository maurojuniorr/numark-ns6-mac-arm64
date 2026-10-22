#include "../audio-out/AudioOutputRing.hpp"
#include "../audio-out/AudioPacketPolicy.hpp"
#include "../audio-out/NS6AudioOutLayout.hpp"
#include "../audio-out/AudioOutPacket.hpp"
#include "../audio-out/AudioOutTransferBatch.hpp"
#include "../audio-out/AudioOutSchedule.hpp"
#include "../audio-out/AudioOutTransferSlots.hpp"

#include <cassert>
#include <array>
#include <cstdint>
#include <thread>
#include <stdexcept>
#include <vector>

namespace {

void testEmptyAndPartialRead() {
    AudioOutputRing ring(4, 2);
    float output[8]{};
    assert(ring.read(output, 4) == 0);

    const float input[] = {1, -1, 2, -2, 3, -3};
    assert(ring.write(input, 3) == 3);
    assert(ring.read(output, 4) == 3);
    for (int i = 0; i < 6; ++i) assert(output[i] == input[i]);
}

void testResampledReadInterpolatesAndPreservesFractionalPhase() {
    AudioOutputRing ring(8, 1);
    const float input[] = {0, 1, 2, 3};
    float output[4]{};
    assert(ring.write(input, 4) == 4);
    assert(ring.readResampled(output, 2, 0.5) == 2);
    assert(output[0] == 0 && output[1] == 0.5f);
    assert(ring.availableFrames() == 3);
    assert(ring.readResampled(output + 2, 2, 1.0) == 2);
    assert(output[2] == 1 && output[3] == 2);
}

void testWraparoundAndFullRing() {
    AudioOutputRing ring(3, 1);
    const float first[] = {1, 2};
    const float second[] = {3, 4, 5};
    float output[3]{};

    assert(ring.write(first, 2) == 2);
    assert(ring.read(output, 2) == 2);
    assert(output[0] == 1 && output[1] == 2);
    assert(ring.write(second, 3) == 3);
    assert(ring.write(first, 1) == 0);
    assert(ring.read(output, 3) == 3);
    assert(output[0] == 3 && output[1] == 4 && output[2] == 5);
}

void testConcurrentFrameOrder() {
    constexpr std::uint32_t totalFrames = 500000;
    AudioOutputRing ring(257, 2);
    std::thread producer([&] {
        std::uint32_t next = 0;
        float input[22];
        while (next < totalFrames) {
            const std::uint32_t count = (totalFrames - next < 11) ? totalFrames - next : 11;
            for (std::uint32_t i = 0; i < count; ++i) {
                input[i * 2] = static_cast<float>(next + i);
                input[i * 2 + 1] = -static_cast<float>(next + i);
            }
            next += ring.write(input, count);
            if (next < totalFrames) std::this_thread::yield();
        }
    });

    std::uint32_t received = 0;
    float output[34];
    while (received < totalFrames) {
        const std::uint32_t count = ring.read(output, 17);
        for (std::uint32_t i = 0; i < count; ++i) {
            assert(output[i * 2] == static_cast<float>(received + i));
            assert(output[i * 2 + 1] == -static_cast<float>(received + i));
        }
        received += count;
        if (count == 0) std::this_thread::yield();
    }
    producer.join();
}

void testPacketPolicyPreservesFractionalSampleRate() {
    AudioPacketPolicy policy(44100, 1000);
    std::uint32_t totalFrames = 0;
    std::uint32_t fortyFiveFramePackets = 0;
    for (std::uint32_t interval = 0; interval < 1000; ++interval) {
        const auto frames = policy.nextPacketFrames();
        assert(frames == 44 || frames == 45);
        totalFrames += frames;
        fortyFiveFramePackets += frames == 45;
    }
    assert(totalFrames == 44100);
    assert(fortyFiveFramePackets == 100);
}

void testPacketPolicySupportsMicroframeServiceRate() {
    AudioPacketPolicy policy(44100, 8000);
    std::uint32_t totalFrames = 0;
    for (std::uint32_t interval = 0; interval < 8000; ++interval) {
        const auto frames = policy.nextPacketFrames();
        assert(frames == 5 || frames == 6);
        totalFrames += frames;
    }
    assert(totalFrames == 44100);
}

void testPacketPolicyUsesValidatedNS6FeedbackPattern() {
    AudioPacketPolicy policy(44100, 8000);
    policy.observeFeedback(46);
    policy.observeFeedback(44);
    std::uint32_t totalFrames = 0;
    const std::array<std::uint32_t, 8> expected{5, 6, 6, 6, 5, 6, 6, 6};
    for (const auto frames : expected) {
        assert(policy.nextPacketFrames() == frames);
        totalFrames += frames;
    }
    assert(totalFrames == 46);

    policy.observeFeedback(0xaa);
    totalFrames = 0;
    for (int i = 0; i < 8; ++i) totalFrames += policy.nextPacketFrames();
    assert(totalFrames == 44);
}

void testInvalidFeedbackUsesTheNextHistoryPattern() {
    AudioPacketPolicy policy(44100, 8000);
    policy.observeFeedback(46);
    for (int i = 0; i < 8; ++i) (void)policy.nextPacketFrames();

    policy.observeInvalidFeedback();
    const std::array<std::uint32_t, 8> predicted{5, 6, 5, 6, 5, 6, 5, 6};
    for (const auto frames : predicted) assert(policy.nextPacketFrames() == frames);
}

void testPacketPatternObserverLosesAndReacquiresLock() {
    AudioPacketPolicy policy(44100, 8000);
    for (int i = 0; i < 16; ++i) {
        policy.observeFeedback(44);
        for (int packet = 0; packet < 8; ++packet) (void)policy.nextPacketFrames();
    }
    assert(policy.patternLocked());
    policy.observeInvalidFeedback();
    for (int packet = 0; packet < 8; ++packet) (void)policy.nextPacketFrames();
    assert(!policy.patternLocked());
    for (int i = 0; i < 14; ++i) {
        policy.observeFeedback(44);
        for (int packet = 0; packet < 8; ++packet) (void)policy.nextPacketFrames();
    }
    assert(!policy.patternLocked());
    policy.observeFeedback(44);
    for (int packet = 0; packet < 8; ++packet) (void)policy.nextPacketFrames();
    assert(policy.patternLocked());
}

void testAdaptiveClockTracksFeedbackAndBoundsQueueTrim() {
    AudioPacketPolicy policy(44100, 8000);
    assert(policy.sourceFramesPerOutputFrame(4096) == 1.0);
    assert(policy.sourceFramesPerOutputFrame(0) == 0.995);
    assert(policy.sourceFramesPerOutputFrame(8192) == 1.005);
    for (std::uint32_t i = 0; i < AudioPacketPolicy::kFeedbackWindow; ++i)
        policy.observeFeedback(45);
    const double measuredStep = policy.sourceFramesPerOutputFrame(4096);
    assert(policy.measuredClockMilliHz() == 45000000);
    assert(policy.effectiveClockMilliHz() == 44100300);
    assert(measuredStep > 0.99998 && measuredStep < 1.0);
    assert(policy.sourceFramesPerOutputFrame(8192) > measuredStep);
}

void testPacketPolicyRejectsZeroRates() {
    bool rejected = false;
    try {
        AudioPacketPolicy policy(44100, 0);
        (void)policy;
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    assert(rejected);
}

void testMeasuredNS6AudioOutLayout() {
    const NS6AudioOutEndpointDescriptor output{
        0x15e4, 0x0079, USBSpeed::High, 0, 1, 0x02,
        USBTransferType::Isochronous, 156, 1};
    assert(ns6AudioOutServiceRateHz(output) == 8000);
}

void testAudioOutLayoutRejectsUnexpectedEndpoint() {
    auto output = NS6AudioOutEndpointDescriptor{
        0x15e4, 0x0079, USBSpeed::High, 0, 1, 0x02,
        USBTransferType::Isochronous, 156, 1};
    output.endpointAddress = 0x04;
    assert(ns6AudioOutServiceRateHz(output) == 0);
    output.endpointAddress = 0x02;
    output.maxPacketSize = 64;
    assert(ns6AudioOutServiceRateHz(output) == 0);
    output.maxPacketSize = 156;
    output.speed = USBSpeed::Full;
    assert(ns6AudioOutServiceRateHz(output) == 0);
}

void testPacketPacksFourChannelSigned24BitLittleEndian() {
    AudioOutputRing ring(8, 4);
    AudioPacketPolicy policy(44100, 8000);
    AudioOutPacket packet;
    const float input[6 * 4] = {
        1.0f, -1.0f, 0.5f, -0.5f, 1.0f, -1.0f, 0.5f, -0.5f,
        1.0f, -1.0f, 0.5f, -0.5f, 1.0f, -1.0f, 0.5f, -0.5f,
        1.0f, -1.0f, 0.5f, -0.5f, 1.0f, -1.0f, 0.5f, -0.5f};
    assert(ring.write(input, 6) == 6);
    assert(packet.build(ring, policy) == 6);
    const std::uint8_t expected[] = {
        0xff, 0xff, 0x7f, 0x00, 0x00, 0x80,
        0x00, 0x00, 0x40, 0x00, 0x00, 0xc0};
    for (std::size_t i = 0; i < packet.byteCount(); ++i)
        assert(packet.bytes()[i] == expected[i % sizeof(expected)]);
}

void testPacketSilencesMissingFramesAndClampsInvalidFloat() {
    AudioOutputRing ring(2, 4);
    AudioPacketPolicy policy(44100, 8000);
    AudioOutPacket packet;
    const float input[] = {2.0f, -2.0f, 0.0f, 0.0f};
    assert(ring.write(input, 1) == 1);
    assert(packet.build(ring, policy) == 6);
    assert(packet.missingFrames() != 0);
    assert(packet.bytes()[0] == 0xff && packet.bytes()[1] == 0xff && packet.bytes()[2] == 0x7f);
    assert(packet.bytes()[3] == 0x00 && packet.bytes()[4] == 0x00 && packet.bytes()[5] == 0x80);
    for (std::size_t i = 24; i < packet.byteCount(); ++i) assert(packet.bytes()[i] == 0);
}

void testTransferSlotsFillUpAndReuseOnlyAfterCompletion() {
    AudioOutputRing ring(2048, 4);
    AudioPacketPolicy policy(44100, 8000);
    AudioOutTransferSlots slots;
    for (std::uint32_t i = 0; i < AudioOutTransferSlots::kSlotCount; ++i) {
        assert(slots.reserve(ring, policy) == i);
    }
    assert(slots.reserve(ring, policy) == AudioOutTransferSlots::kNoSlot);
    assert(slots.inFlightCount() == AudioOutTransferSlots::kSlotCount);
    assert(slots.complete(3));
    assert(slots.reserve(ring, policy) == 3);
    assert(!slots.complete(AudioOutTransferSlots::kSlotCount));
}

void testTransferSlotsStopWithoutReleasingPendingBuffers() {
    AudioOutputRing ring(2048, 4);
    AudioPacketPolicy policy(44100, 8000);
    AudioOutTransferSlots slots;
    const auto slot = slots.reserve(ring, policy);
    assert(slot != AudioOutTransferSlots::kNoSlot);
    slots.stopAccepting();
    assert(slots.reserve(ring, policy) == AudioOutTransferSlots::kNoSlot);
    assert(slots.inFlightCount() == 1);
    assert(slots.complete(slot));
    assert(slots.inFlightCount() == 0);
}

void testTransferBatchPrepares32MicroframesAhead() {
    AudioOutputRing ring(256, 4);
    AudioPacketPolicy policy(44100, 8000);
    std::array<float, 200 * 4> input{};
    assert(ring.write(input.data(), 200) == 200);
    AudioOutTransferBatch batch;
    assert(batch.build(ring, policy));
    std::uint32_t frames = 0;
    for (std::uint32_t i = 0; i < AudioOutTransferBatch::kServiceIntervals; ++i) {
        const auto bytes = batch.packetByteCount(i);
        assert(bytes == 60 || bytes == 72);
        frames += bytes / 12;
    }
    assert(frames == 176);
    assert(batch.byteCount() == frames * 12);
    assert(batch.packetByteCount(AudioOutTransferBatch::kServiceIntervals) == 0);
}

void testScheduleKeepsTransferStartsContiguous() {
    AudioOutSchedule schedule;
    assert(schedule.begin(100) == 102);
    assert(schedule.takeNext(100).startFrame == 106);
    assert(schedule.takeNext(100).startFrame == 110);
}

void testScheduleSkipsPastStartInsteadOfSubmittingLate() {
    AudioOutSchedule schedule;
    assert(schedule.begin(100) == 102);
    assert(schedule.takeNext(115).startFrame == 118);
    assert(schedule.skippedIntervals() == 3);
}

}  // namespace

int main() {
    testEmptyAndPartialRead();
    testResampledReadInterpolatesAndPreservesFractionalPhase();
    testWraparoundAndFullRing();
    testConcurrentFrameOrder();
    testPacketPolicyPreservesFractionalSampleRate();
    testPacketPolicySupportsMicroframeServiceRate();
    testPacketPolicyUsesValidatedNS6FeedbackPattern();
    testInvalidFeedbackUsesTheNextHistoryPattern();
    testPacketPatternObserverLosesAndReacquiresLock();
    testAdaptiveClockTracksFeedbackAndBoundsQueueTrim();
    testPacketPolicyRejectsZeroRates();
    testMeasuredNS6AudioOutLayout();
    testAudioOutLayoutRejectsUnexpectedEndpoint();
    testPacketPacksFourChannelSigned24BitLittleEndian();
    testPacketSilencesMissingFramesAndClampsInvalidFloat();
    testTransferSlotsFillUpAndReuseOnlyAfterCompletion();
    testTransferSlotsStopWithoutReleasingPendingBuffers();
    testTransferBatchPrepares32MicroframesAhead();
    testScheduleKeepsTransferStartsContiguous();
    testScheduleSkipsPastStartInsteadOfSubmittingLate();
}
