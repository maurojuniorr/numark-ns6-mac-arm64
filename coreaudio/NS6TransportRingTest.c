#include "NS6Transport.h"
#include <assert.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static AudioStreamBasicDescription float_format(void) {
    AudioStreamBasicDescription format = {0};
    format.mSampleRate = 44100.0;
    format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagsNativeEndian | kAudioFormatFlagIsPacked;
    format.mBytesPerPacket = 16;
    format.mFramesPerPacket = 1;
    format.mBytesPerFrame = 16;
    format.mChannelsPerFrame = 4;
    format.mBitsPerChannel = 32;
    return format;
}

static void fill(float *frames, UInt32 count, float first) {
    for (UInt32 i = 0; i < count * 4; ++i) frames[i] = first + (float)i / 100.0f;
}

static void expect_frame(const unsigned char *actual, const float *expected) {
    for (unsigned channel = 0; channel < 4; ++channel) {
        const unsigned char *sample = actual + channel * 3;
        int32_t value = (int32_t)sample[0] | ((int32_t)sample[1] << 8) |
                        ((int32_t)(int8_t)sample[2] << 16);
        assert(fabsf((float)value / 8388607.0f - expected[channel]) < 0.000001f);
    }
}

static void test_timestamped_sequential_playback(void) {
    AudioStreamBasicDescription format = float_format();
    float input[16]; unsigned char output[48] = {0};
    fill(input, 4, 0.1f);
    ns6_transport_reset();
    assert(ns6_transport_enqueue_at(input, 100, 4, &format));
    assert(ns6_transport_available() == 4);
    assert(ns6_transport_dequeue(output, 4) == 4);
    for (unsigned frame = 0; frame < 4; ++frame) expect_frame(output + frame * 12, input + frame * 4);
}

static void test_timestamped_write_wraps_at_ring_end(void) {
    AudioStreamBasicDescription format = float_format();
    float input[16]; unsigned char output[48] = {0};
    fill(input, 4, 0.2f);
    ns6_transport_reset();
    assert(ns6_transport_enqueue_at(input, NS6_TRANSPORT_RING_FRAMES - 2, 4, &format));
    assert(ns6_transport_available() == 4);
    assert(ns6_transport_dequeue(output, 4) == 4);
    for (unsigned frame = 0; frame < 4; ++frame) expect_frame(output + frame * 12, input + frame * 4);
}

static void test_timestamped_cycles_are_appended_in_arrival_order(void) {
    AudioStreamBasicDescription format = float_format();
    float first[16], second[16]; unsigned char output[96] = {0};
    fill(first, 4, 0.1f);
    fill(second, 4, 0.7f);
    ns6_transport_reset();
    assert(ns6_transport_enqueue_at(first, 200, 4, &format));
    assert(ns6_transport_enqueue_at(second, 202, 4, &format));
    assert(ns6_transport_available() == 8);
    assert(ns6_transport_dequeue(output, 8) == 8);
    expect_frame(output, first);
    expect_frame(output + 12, first + 4);
    for (unsigned frame = 0; frame < 4; ++frame) expect_frame(output + (frame + 4) * 12, second + frame * 4);
}

static void test_timestamp_gap_does_not_insert_silence_in_usb_fifo(void) {
    AudioStreamBasicDescription format = float_format();
    float first[8], second[8]; unsigned char output[48] = {0}, later[24] = {0};
    fill(first, 2, 0.1f);
    fill(second, 2, 0.8f);
    ns6_transport_reset();
    assert(ns6_transport_enqueue_at(first, 300, 2, &format));
    assert(ns6_transport_enqueue_at(second, 304, 2, &format));
    assert(ns6_transport_available() == 4);
    UInt32 received = ns6_transport_dequeue(output, 4);
    assert(received == 4);
    for (unsigned frame = 0; frame < 2; ++frame) expect_frame(output + frame * 12, first + frame * 4);
    for (unsigned frame = 0; frame < 2; ++frame) expect_frame(output + (frame + 2) * 12, second + frame * 4);
    (void)later;
}

static void test_late_timestamp_audio_appends_after_consumed_audio(void) {
    AudioStreamBasicDescription format = float_format();
    float input[16]; unsigned char output[48] = {0};
    fill(input, 4, 0.1f);
    ns6_transport_reset();
    assert(ns6_transport_enqueue_at(input, 400, 4, &format));
    assert(ns6_transport_dequeue(output, 4) == 4);
    assert(ns6_transport_enqueue_at(input, 401, 2, &format));
    assert(ns6_transport_available() == 2);
}

static void test_coreaudio_timestamp_jump_does_not_create_usb_fifo_hole(void) {
    AudioStreamBasicDescription format = float_format();
    float first[16], second[16]; unsigned char output[96] = {0};
    fill(first, 4, 0.1f);
    fill(second, 4, 0.5f);
    ns6_transport_reset();
    assert(ns6_transport_enqueue_at(first, 100, 4, &format));
    /* CoreAudio can rebase/adjust its output timeline independently of USB. */
    assert(ns6_transport_enqueue_at(second, 10000, 4, &format));
    assert(ns6_transport_available() == 8);
    assert(ns6_transport_dequeue(output, 8) == 8);
    for (unsigned frame = 0; frame < 4; ++frame) expect_frame(output + frame * 12, first + frame * 4);
    for (unsigned frame = 0; frame < 4; ++frame) expect_frame(output + (frame + 4) * 12, second + frame * 4);
}

static void test_fifo_compatibility_uses_monotonic_timestamps(void) {
    AudioStreamBasicDescription format = float_format();
    float first[8], second[8]; unsigned char output[48] = {0};
    fill(first, 2, 0.1f);
    fill(second, 2, 0.5f);
    ns6_transport_reset();
    assert(ns6_transport_enqueue(first, 2, &format));
    assert(ns6_transport_enqueue(second, 2, &format));
    assert(ns6_transport_available() == 4);
    assert(ns6_transport_dequeue(output, 4) == 4);
    for (unsigned frame = 0; frame < 2; ++frame) expect_frame(output + frame * 12, first + frame * 4);
    for (unsigned frame = 0; frame < 2; ++frame) expect_frame(output + (frame + 2) * 12, second + frame * 4);
}

static void test_multiple_clients_share_arrival_order_fifo(void) {
    AudioStreamBasicDescription format = float_format();
    float first_client[8], second_client[8];
    unsigned char output[48] = {0};
    fill(first_client, 2, 0.1f);
    fill(second_client, 2, 0.7f);
    ns6_transport_reset();
    assert(ns6_transport_enqueue(first_client, 2, &format));
    assert(ns6_transport_enqueue(second_client, 2, &format));
    assert(ns6_transport_available() == 4);
    assert(ns6_transport_dequeue(output, 4) == 4);
    for (unsigned frame = 0; frame < 2; ++frame)
        expect_frame(output + frame * 12, first_client + frame * 4);
    for (unsigned frame = 0; frame < 2; ++frame)
        expect_frame(output + (frame + 2) * 12, second_client + frame * 4);
}

static void test_multiple_clients_are_mixed_at_their_common_sample_time(void) {
    AudioStreamBasicDescription format = float_format();
    float first[8], second[8];
    unsigned char output[24] = {0};
    NS6TransportShared shared;
    NS6TransportReader reader = {0};
    ns6_transport_init_shared(&shared);
    fill(first, 2, 0.2f);
    fill(second, 2, 0.3f);
    assert(ns6_transport_mix_at_shared(&shared, first, 1000, 2, &format));
    assert(ns6_transport_mix_at_shared(&shared, second, 1000, 2, &format));
    assert(ns6_transport_available_shared(&shared) == 2);
    assert(ns6_transport_dequeue_shared(&shared, &reader, output, 2) == 2);
    for (unsigned frame = 0; frame < 2; ++frame) {
        float expected_frame[4];
        for (unsigned channel = 0; channel < 4; ++channel) {
            expected_frame[channel] = first[frame * 4 + channel] + second[frame * 4 + channel];
        }
        expect_frame(output + frame * 12, expected_frame);
    }
}

static void test_dequeue_reports_a_timestamp_hole_separately_from_empty_queue(void) {
    AudioStreamBasicDescription format = float_format();
    NS6TransportShared shared;
    NS6TransportReader reader = {0};
    float first[8], second[8];
    unsigned char output[48] = {0};
    NS6TransportStopReason reason = NS6_TRANSPORT_STOP_NONE;
    ns6_transport_init_shared(&shared);
    fill(first, 2, 0.2f);
    fill(second, 2, 0.3f);
    assert(ns6_transport_mix_at_shared(&shared, first, 1000, 2, &format));
    assert(ns6_transport_mix_at_shared(&shared, second, 1004, 2, &format));
    assert(ns6_transport_dequeue_resampled_shared_diagnostic(
               &shared, &reader, output, 4, 1.0, &reason) == 2);
    assert(reason == NS6_TRANSPORT_STOP_MISSING_TAG);
}

static void test_dequeue_reports_queue_end_separately_from_missing_tag(void) {
    AudioStreamBasicDescription format = float_format();
    NS6TransportShared shared;
    NS6TransportReader reader = {0};
    float input[8];
    unsigned char output[48] = {0};
    NS6TransportStopReason reason = NS6_TRANSPORT_STOP_NONE;
    ns6_transport_init_shared(&shared);
    fill(input, 2, 0.2f);
    assert(ns6_transport_enqueue_shared(&shared, input, 2, &format));
    assert(ns6_transport_dequeue_resampled_shared_diagnostic(
               &shared, &reader, output, 4, 1.0, &reason) == 2);
    assert(reason == NS6_TRANSPORT_STOP_EMPTY);
}

static void test_next_stream_continues_shared_sample_timeline(void) {
    AudioStreamBasicDescription format = float_format();
    NS6TransportShared shared;
    NS6TransportReader reader = {0};
    float first[16], second[16];
    unsigned char output[48] = {0};
    ns6_transport_init_shared(&shared);
    fill(first, 4, 0.1f);
    fill(second, 4, 0.5f);

    assert(ns6_transport_mix_at_shared(&shared, first, 0, 4, &format));
    assert(ns6_transport_dequeue_shared(&shared, &reader, output, 4) == 4);
    assert(ns6_transport_available_shared(&shared) == 0);

    /* A stopped CoreAudio client must resume on the existing USB read cursor. */
    assert(ns6_transport_mix_at_shared(&shared, second, 4, 4, &format));
    assert(ns6_transport_available_shared(&shared) == 4);
    assert(ns6_transport_dequeue_shared(&shared, &reader, output, 4) == 4);
    for (unsigned frame = 0; frame < 4; ++frame)
        expect_frame(output + frame * 12, second + frame * 4);
}

static void test_empty_readable_queue_rebases_after_a_stale_timeline_gap(void) {
    AudioStreamBasicDescription format = float_format();
    NS6TransportShared shared;
    NS6TransportReader reader = {0};
    float first[8], after_gap[8], resumed[8];
    unsigned char output[24] = {0};
    ns6_transport_init_shared(&shared);
    fill(first, 2, 0.1f);
    fill(after_gap, 2, 0.3f);
    fill(resumed, 2, 0.7f);

    assert(ns6_transport_mix_at_shared(&shared, first, 1000, 2, &format));
    assert(ns6_transport_mix_at_shared(&shared, after_gap, 1004, 2, &format));
    assert(ns6_transport_dequeue_shared(&shared, &reader, output, 2) == 2);
    assert(ns6_transport_available_shared(&shared) == 0);

    assert(ns6_transport_mix_at_shared(&shared, resumed, 20000, 2, &format));
    assert(ns6_transport_available_shared(&shared) == 2);
    assert(ns6_transport_dequeue_shared(&shared, &reader, output, 2) == 2);
    expect_frame(output, resumed);
}

static void test_empty_shared_ring_recovers_from_coreaudio_timeline_jump(void) {
    AudioStreamBasicDescription format = float_format();
    NS6TransportShared shared;
    NS6TransportReader reader = {0};
    float first[16], next[16];
    unsigned char output[48] = {0};
    ns6_transport_init_shared(&shared);
    fill(first, 4, 0.1f);
    fill(next, 4, 0.5f);

    assert(ns6_transport_mix_at_shared(&shared, first, 1000, 4, &format));
    assert(ns6_transport_dequeue_shared(&shared, &reader, output, 4) == 4);
    ns6_transport_advance_shared(&shared, 20000);
    assert(ns6_transport_available_shared(&shared) == 0);

    assert(ns6_transport_mix_at_shared(&shared, next, 15000000, 4, &format));
    assert(ns6_transport_available_shared(&shared) == 4);
    assert(ns6_transport_dequeue_shared(&shared, &reader, output, 4) == 4);
    for (unsigned frame = 0; frame < 4; ++frame)
        expect_frame(output + frame * 12, next + frame * 4);
}

typedef struct {
    NS6TransportShared *shared;
    float *frames;
    UInt64 sample_time;
    UInt32 count;
    bool accepted;
} ConcurrentMix;

static void *enqueue_concurrent_mix(void *opaque) {
    ConcurrentMix *mix = opaque;
    AudioStreamBasicDescription format = float_format();
    mix->accepted = ns6_transport_mix_at_shared(mix->shared, mix->frames,
                                                 mix->sample_time, mix->count, &format);
    return NULL;
}

static void test_concurrent_clients_add_without_losing_samples(void) {
    enum { FRAMES = 2048 };
    NS6TransportShared shared;
    NS6TransportReader reader = {0};
    float *first = malloc((size_t)FRAMES * 4 * sizeof(float));
    float *second = malloc((size_t)FRAMES * 4 * sizeof(float));
    unsigned char *output = malloc((size_t)FRAMES * 12);
    assert(first && second && output);
    for (unsigned frame = 0; frame < FRAMES; ++frame) {
        for (unsigned channel = 0; channel < 4; ++channel) {
            first[frame * 4 + channel] = 0.2f;
            second[frame * 4 + channel] = 0.3f;
        }
    }
    ns6_transport_init_shared(&shared);
    ConcurrentMix a = {&shared, first, 50000, FRAMES, false};
    ConcurrentMix b = {&shared, second, 50000, FRAMES, false};
    pthread_t first_thread, second_thread;
    assert(pthread_create(&first_thread, NULL, enqueue_concurrent_mix, &a) == 0);
    assert(pthread_create(&second_thread, NULL, enqueue_concurrent_mix, &b) == 0);
    assert(pthread_join(first_thread, NULL) == 0);
    assert(pthread_join(second_thread, NULL) == 0);
    assert(a.accepted && b.accepted);
    assert(ns6_transport_available_shared(&shared) == FRAMES);
    assert(ns6_transport_dequeue_shared(&shared, &reader, output, FRAMES) == FRAMES);
    const float expected[4] = {0.5f, 0.5f, 0.5f, 0.5f};
    for (unsigned frame = 0; frame < FRAMES; ++frame)
        expect_frame(output + frame * 12, expected);
    free(output);
    free(second);
    free(first);
}

static float decode_sample(const unsigned char *sample) {
    int32_t value = (int32_t)sample[0] | ((int32_t)sample[1] << 8) |
                    ((int32_t)(int8_t)sample[2] << 16);
    return (float)value / 8388607.0f;
}

static void test_fractional_dequeue_interpolates_and_carries_phase(void) {
    AudioStreamBasicDescription format = float_format();
    float input[40];
    unsigned char output[48] = {0};
    fill(input, 10, 0.1f);
    ns6_transport_reset();
    assert(ns6_transport_enqueue(input, 10, &format));
    assert(ns6_transport_dequeue_resampled(output, 4, 1.5) == 4);
    for (unsigned channel = 0; channel < 4; ++channel) {
        float expected_first = input[channel];
        float expected_second = (input[4 + channel] + input[8 + channel]) * 0.5f;
        assert(fabsf(decode_sample(output + channel * 3) - expected_first) < 0.000001f);
        assert(fabsf(decode_sample(output + 12 + channel * 3) - expected_second) < 0.000001f);
    }
    assert(ns6_transport_available() == 4);
    assert(ns6_transport_dequeue_resampled(output, 2, 1.5) == 2);
    assert(ns6_transport_available() == 1);
}

static void test_small_clock_offset_is_absorbed_fractionally(void) {
    AudioStreamBasicDescription format = float_format();
    float input[1200 * 4];
    unsigned char output[1000 * 12];
    fill(input, 1200, 0.0f);
    ns6_transport_reset();
    assert(ns6_transport_enqueue(input, 1200, &format));
    assert(ns6_transport_dequeue_resampled(output, 1000, 1.0011) == 1000);
    assert(ns6_transport_available() == 199);
}

static void test_new_audio_recovers_after_read_cursor_passed_empty_queue(void) {
    AudioStreamBasicDescription format = float_format();
    float input[8];
    unsigned char output[24] = {0};
    fill(input, 2, 0.3f);
    ns6_transport_reset();
    assert(ns6_transport_enqueue(input, 2, &format));
    assert(ns6_transport_dequeue(output, 2) == 2);
    assert(ns6_transport_advance(5) == 5);
    assert(ns6_transport_enqueue(input, 2, &format));
    assert(ns6_transport_available() == 2);
}

int main(void) {
    test_timestamped_sequential_playback();
    test_timestamped_write_wraps_at_ring_end();
    test_timestamped_cycles_are_appended_in_arrival_order();
    test_timestamp_gap_does_not_insert_silence_in_usb_fifo();
    test_late_timestamp_audio_appends_after_consumed_audio();
    test_coreaudio_timestamp_jump_does_not_create_usb_fifo_hole();
    test_fifo_compatibility_uses_monotonic_timestamps();
    test_multiple_clients_share_arrival_order_fifo();
    test_multiple_clients_are_mixed_at_their_common_sample_time();
    test_dequeue_reports_a_timestamp_hole_separately_from_empty_queue();
    test_dequeue_reports_queue_end_separately_from_missing_tag();
    test_next_stream_continues_shared_sample_timeline();
    test_empty_readable_queue_rebases_after_a_stale_timeline_gap();
    test_empty_shared_ring_recovers_from_coreaudio_timeline_jump();
    test_concurrent_clients_add_without_losing_samples();
    test_fractional_dequeue_interpolates_and_carries_phase();
    test_small_clock_offset_is_absorbed_fractionally();
    test_new_audio_recovers_after_read_cursor_passed_empty_queue();
    puts("NS6 timestamp-ring transport tests passed");
    return 0;
}
