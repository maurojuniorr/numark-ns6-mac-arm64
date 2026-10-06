#include "NS6ClockControl.h"

#include <assert.h>
#include <stdio.h>

static void test_nominal_clock_emits_44100_frames_per_second(void) {
    NS6ClockControl control;
    ns6_clock_control_init(&control);
    uint64_t frames = 0;
    for (unsigned ms = 0; ms < 1000; ++ms) {
        unsigned packet_frames[8];
        ns6_clock_control_next_millisecond(&control, packet_frames);
        for (unsigned i = 0; i < 8; ++i) frames += packet_frames[i];
    }
    assert(frames == 44100);
    assert(!ns6_clock_control_is_locked(&control));
}

static void test_feedback_tracks_measured_hardware_rate(void) {
    NS6ClockControl control;
    ns6_clock_control_init(&control);
    uint64_t expected = 0;
    for (unsigned i = 0; i < 40000; ++i) {
        /* Spread measured 45-frame reports through 40 s, not in a burst. */
        unsigned requested = ((uint64_t)(i + 1) * 4012 / 40000) !=
                             ((uint64_t)i * 4012 / 40000) ? 45 : 44;
        unsigned packet_frames[8];
        ns6_clock_control_observe(&control, (uint8_t)requested);
        ns6_clock_control_next_millisecond(&control, packet_frames);
        if (i == 9999)
            assert(ns6_clock_control_rate_millihz(&control) == 44100000);
        expected += requested;
        unsigned total = 0;
        for (unsigned packet = 0; packet < 8; ++packet) {
            assert(packet_frames[packet] == 5 || packet_frames[packet] == 6);
            total += packet_frames[packet];
        }
        assert(total >= 40 && total <= 48);
    }
    assert(ns6_clock_control_is_locked(&control));
    assert(ns6_clock_control_requested_total(&control) == expected);
    assert(ns6_clock_control_sent_total(&control) > 0);
    assert(ns6_clock_control_rate_millihz(&control) == 44100300);
    double source_step = ns6_clock_control_source_frames_per_output_frame(&control, 4096);
    assert(source_step < 1.0 && source_step > 0.999);
    double high_queue_step = ns6_clock_control_source_frames_per_output_frame(&control, 16000);
    double low_queue_step = ns6_clock_control_source_frames_per_output_frame(&control, 0);
    assert(high_queue_step > 1.0015);
    assert(high_queue_step <= 1.005);
    assert(low_queue_step < source_step && low_queue_step >= 0.995);
}

static void test_feedback_packet_sizes_are_distributed_within_each_millisecond(void) {
    NS6ClockControl control;
    ns6_clock_control_init(&control);
    const unsigned expected_44[8] = {5, 6, 5, 6, 5, 6, 5, 6};
    const unsigned expected_45[8] = {5, 6, 5, 6, 6, 5, 6, 6};
    unsigned packet_frames[8];

    for (unsigned ms = 0; ms < 9; ++ms) {
        ns6_clock_control_next_millisecond(&control, packet_frames);
        unsigned total = 0;
        for (unsigned packet = 0; packet < 8; ++packet) total += packet_frames[packet];
        assert(total == 44);
        if (ms == 0)
            for (unsigned packet = 0; packet < 8; ++packet)
                assert(packet_frames[packet] == expected_44[packet]);
    }
    ns6_clock_control_next_millisecond(&control, packet_frames);
    for (unsigned packet = 0; packet < 8; ++packet)
        assert(packet_frames[packet] == expected_45[packet]);
}

static void test_invalid_feedback_cannot_change_rate(void) {
    NS6ClockControl control;
    ns6_clock_control_init(&control);
    for (unsigned i = 0; i < 8000; ++i)
        ns6_clock_control_observe(&control, 63);
    assert(!ns6_clock_control_is_locked(&control));
    assert(ns6_clock_control_rate_millihz(&control) == 44100000);
}

static void test_feedback_cadence_follows_device_report_without_debt_bias(void) {
    NS6ClockControl control;
    ns6_clock_control_init(&control);
    uint64_t expected = 0;
    for (unsigned i = 0; i < 100000; ++i) {
        uint8_t requested = (i % 10u == 9u) ? 45 : 44;
        ns6_clock_control_observe(&control, requested);
        unsigned packet_frames[8];
        ns6_clock_control_next_millisecond(&control, packet_frames);
        unsigned total = 0;
        for (unsigned packet = 0; packet < 8; ++packet) {
            assert(packet_frames[packet] == 5 || packet_frames[packet] == 6);
            total += packet_frames[packet];
        }
        assert(total == requested);
        expected += requested;
    }
    assert(ns6_clock_control_requested_total(&control) == expected);
    assert(ns6_clock_control_sent_total(&control) == expected);
}

static void test_batched_feedback_uses_fractional_rate_instead_of_repeating_last_report(void) {
    NS6ClockControl control;
    ns6_clock_control_init(&control);
    unsigned packet_frames[8];

    /* Model four milliseconds of reports arriving before the next four-ms
       USB output block is prepared. 45-frame reports are deliberately
       placed so the final report in every block is 44. */
    for (unsigned batch = 0; batch < 20000; ++batch) {
        for (unsigned ms = 0; ms < 4; ++ms) {
            unsigned index = batch * 4 + ms;
            ns6_clock_control_observe(&control, (uint8_t)(index % 10u == 9u ? 45 : 44));
        }
        for (unsigned ms = 0; ms < 4; ++ms)
            ns6_clock_control_next_millisecond(&control, packet_frames);
    }

    uint64_t requested_at_lock = ns6_clock_control_requested_total(&control);
    uint64_t sent_at_lock = ns6_clock_control_sent_total(&control);
    for (unsigned batch = 0; batch < 10000; ++batch) {
        for (unsigned ms = 0; ms < 4; ++ms) {
            unsigned index = 80000 + batch * 4 + ms;
            ns6_clock_control_observe(&control, (uint8_t)(index % 10u == 9u ? 45 : 44));
        }
        for (unsigned ms = 0; ms < 4; ++ms)
            ns6_clock_control_next_millisecond(&control, packet_frames);
    }

    uint64_t requested_delta = ns6_clock_control_requested_total(&control) - requested_at_lock;
    uint64_t sent_delta = ns6_clock_control_sent_total(&control) - sent_at_lock;
    assert(requested_delta == 1764000);
    assert(sent_delta == requested_delta);
}

static void test_output_rate_is_bounded_to_measured_ns6_clock_band(void) {
    NS6ClockControl control;
    ns6_clock_control_init(&control);
    unsigned packet_frames[8];
    uint64_t output_frames = 0;

    for (unsigned i = 0; i < NS6_FEEDBACK_WINDOW; ++i)
        ns6_clock_control_observe(&control, 44);
    assert(ns6_clock_control_rate_millihz(&control) == 44000000);
    assert(ns6_clock_control_output_rate_millihz(&control) == 44000000);
    for (unsigned ms = 0; ms < 1000; ++ms) {
        ns6_clock_control_next_millisecond(&control, packet_frames);
        for (unsigned packet = 0; packet < 8; ++packet) output_frames += packet_frames[packet];
    }
    assert(output_frames == 44000);

    ns6_clock_control_reset_feedback(&control);
    output_frames = 0;
    for (unsigned i = 0; i < NS6_FEEDBACK_WINDOW; ++i)
        ns6_clock_control_observe(&control, i % 5u == 4u ? 45 : 44);
    assert(ns6_clock_control_rate_millihz(&control) == 44200000);
    assert(ns6_clock_control_output_rate_millihz(&control) == 44100300);
    for (unsigned ms = 0; ms < 10000; ++ms) {
        ns6_clock_control_next_millisecond(&control, packet_frames);
        for (unsigned packet = 0; packet < 8; ++packet) output_frames += packet_frames[packet];
    }
    assert(output_frames == 441003);
}

static void test_sustained_44_feedback_does_not_accumulate_frame_debt(void) {
    NS6ClockControl control;
    ns6_clock_control_init(&control);
    unsigned packet_frames[8];
    uint64_t output_frames = 0;

    for (unsigned i = 0; i < NS6_FEEDBACK_WINDOW; ++i)
        ns6_clock_control_observe(&control, 44);
    uint64_t requested_start = ns6_clock_control_requested_total(&control);
    uint64_t sent_start = ns6_clock_control_sent_total(&control);
    int64_t debt_start = ns6_clock_control_debt(&control);

    for (unsigned ms = 0; ms < 1000; ++ms) {
        ns6_clock_control_observe(&control, 44);
        ns6_clock_control_next_millisecond(&control, packet_frames);
        for (unsigned packet = 0; packet < 8; ++packet)
            output_frames += packet_frames[packet];
    }

    assert(output_frames == 44000);
    assert(ns6_clock_control_requested_total(&control) - requested_start == 44000);
    assert(ns6_clock_control_sent_total(&control) - sent_start == 44000);
    assert(ns6_clock_control_debt(&control) == debt_start);
}

static void test_feedback_reset_returns_to_nominal_rate(void) {
    NS6ClockControl control;
    ns6_clock_control_init(&control);
    for (unsigned i = 0; i < 10000; ++i)
        ns6_clock_control_observe(&control, i < 8800 ? 44 : 45);
    assert(ns6_clock_control_is_locked(&control));

    ns6_clock_control_reset_feedback(&control);
    assert(!ns6_clock_control_is_locked(&control));
    assert(ns6_clock_control_requested_total(&control) == 0);
    assert(ns6_clock_control_sent_total(&control) == 0);
}

static void test_queue_stays_below_overflow_during_60s_rate_mismatch(void) {
    NS6ClockControl control;
    ns6_clock_control_init(&control);
    for (unsigned i = 0; i < NS6_FEEDBACK_WINDOW; ++i)
        ns6_clock_control_observe(&control, i % 10u == 9u ? 45 : 44);

    double queue = 4096.0;
    double producer_fraction = 0.0;
    double source_fraction = 0.0;
    double peak = queue;
    unsigned packets[8];
    for (unsigned ms = 0; ms < 60000; ++ms) {
        producer_fraction += 0.16;
        unsigned produced = 44;
        if (producer_fraction >= 1.0) {
            producer_fraction -= 1.0;
            ++produced;
        }

        ns6_clock_control_next_millisecond(&control, packets);
        unsigned output_frames = 0;
        for (unsigned i = 0; i < 8; ++i) output_frames += packets[i];
        double step = ns6_clock_control_source_frames_per_output_frame(&control,
                                                                        (uint32_t)queue);
        source_fraction += output_frames * step;
        unsigned consumed = (unsigned)source_fraction;
        source_fraction -= consumed;
        queue += (double)produced - (double)consumed;
        if (queue < 0.0) queue = 0.0;
        if (queue > peak) peak = queue;
        assert(queue < 16384.0 && "CoreAudio ring overflowed in rate-mismatch simulation");
    }
    assert(peak < 8192.0 && "adaptive trim failed to recover before ring overflow");
}

static void test_queue_recovers_when_hardware_output_is_44k(void) {
    NS6ClockControl control;
    ns6_clock_control_init(&control);
    for (unsigned i = 0; i < NS6_FEEDBACK_WINDOW; ++i)
        ns6_clock_control_observe(&control, i % 10u == 9u ? 45 : 44);

    double queue = 4096.0;
    double producer_fraction = 0.0;
    double source_fraction = 0.0;
    double peak = queue;
    unsigned ignored_packets[8];
    for (unsigned ms = 0; ms < 180000; ++ms) {
        producer_fraction += 0.16; /* CoreAudio producer: 44,160 frames/s. */
        unsigned produced = 44;
        if (producer_fraction >= 1.0) {
            producer_fraction -= 1.0;
            ++produced;
        }
        ns6_clock_control_next_millisecond(&control, ignored_packets);
        const unsigned output_frames = 44; /* Conservative 44,000-frame/s device cadence. */
        double step = ns6_clock_control_source_frames_per_output_frame(&control,
                                                                        (uint32_t)queue);
        source_fraction += output_frames * step;
        unsigned consumed = (unsigned)source_fraction;
        source_fraction -= consumed;
        queue += (double)produced - (double)consumed;
        if (queue < 0.0) queue = 0.0;
        if (queue > peak) peak = queue;
        assert(queue < 16384.0 && "44 kHz device cadence overflowed the transport ring");
    }
    assert(peak < 8192.0 && "queue trim cannot recover from the measured 44 kHz cadence");
}

int main(void) {
    test_nominal_clock_emits_44100_frames_per_second();
    test_feedback_packet_sizes_are_distributed_within_each_millisecond();
    test_feedback_tracks_measured_hardware_rate();
    test_invalid_feedback_cannot_change_rate();
    test_feedback_cadence_follows_device_report_without_debt_bias();
    test_batched_feedback_uses_fractional_rate_instead_of_repeating_last_report();
    test_output_rate_is_bounded_to_measured_ns6_clock_band();
    test_sustained_44_feedback_does_not_accumulate_frame_debt();
    test_feedback_reset_returns_to_nominal_rate();
    test_queue_stays_below_overflow_during_60s_rate_mismatch();
    test_queue_recovers_when_hardware_output_is_44k();
    puts("NS6 adaptive clock tests passed");
    return 0;
}
