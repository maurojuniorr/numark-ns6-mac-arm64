#include "NS6ClockControl.h"

#include <assert.h>
#include <stdio.h>

static unsigned sum(const unsigned frames[8]) {
    unsigned total = 0;
    for (unsigned packet = 0; packet < 8; ++packet) total += frames[packet];
    return total;
}

static void test_feedback_selects_the_exact_hopper_pattern(void) {
    NS6ClockControl control;
    ns6_clock_control_init(&control);

    const unsigned feedback[] = {42, 43, 44, 45, 46};
    const unsigned expected[5][8] = {
        {6, 5, 5, 5, 6, 5, 5, 5},
        {5, 5, 6, 5, 6, 5, 6, 5},
        {6, 5, 6, 5, 6, 5, 6, 5},
        {6, 6, 5, 6, 5, 6, 5, 6},
        {5, 6, 6, 6, 5, 6, 6, 6},
    };

    for (unsigned pattern = 0; pattern < 5; ++pattern) {
        unsigned packets[8];
        ns6_clock_control_observe(&control, (uint8_t)feedback[pattern]);
        ns6_clock_control_next_millisecond(&control, packets);
        assert(sum(packets) == feedback[pattern]);
        for (unsigned packet = 0; packet < 8; ++packet)
            assert(packets[packet] == expected[pattern][packet]);
    }
    assert(ns6_clock_control_sent_total(&control) == 220);
    assert(ns6_clock_control_requested_total(&control) == 220);
}

static void test_feedback_reports_keep_order_across_a_usb_output_block(void) {
    NS6ClockControl control;
    ns6_clock_control_init(&control);
    const uint8_t reports[] = {44, 45, 44, 46, 43, 42};
    for (unsigned i = 0; i < sizeof(reports); ++i)
        ns6_clock_control_observe(&control, reports[i]);

    for (unsigned i = 0; i < sizeof(reports); ++i) {
        unsigned packets[8];
        ns6_clock_control_next_millisecond(&control, packets);
        assert(sum(packets) == reports[i]);
    }
    assert(ns6_clock_control_sent_total(&control) == 264);
}

static void test_startup_uses_exact_nominal_pattern_until_feedback_arrives(void) {
    NS6ClockControl control;
    ns6_clock_control_init(&control);
    const unsigned pattern_44[8] = {6, 5, 6, 5, 6, 5, 6, 5};
    const unsigned pattern_45[8] = {6, 6, 5, 6, 5, 6, 5, 6};

    for (unsigned ms = 0; ms < 10; ++ms) {
        unsigned packets[8];
        ns6_clock_control_next_millisecond(&control, packets);
        const unsigned *expected = ms == 9 ? pattern_45 : pattern_44;
        for (unsigned packet = 0; packet < 8; ++packet)
            assert(packets[packet] == expected[packet]);
    }
    assert(ns6_clock_control_sent_total(&control) == 441);
}

static void test_invalid_reports_are_ignored_and_reset_discards_pending_patterns(void) {
    NS6ClockControl control;
    ns6_clock_control_init(&control);
    ns6_clock_control_observe(&control, 41);
    ns6_clock_control_observe(&control, 47);
    assert(!ns6_clock_control_is_locked(&control));

    ns6_clock_control_observe(&control, 46);
    ns6_clock_control_reset_feedback(&control);
    unsigned packets[8];
    ns6_clock_control_next_millisecond(&control, packets);
    assert(sum(packets) == 44);
    assert(ns6_clock_control_requested_total(&control) == 0);
    assert(ns6_clock_control_sent_total(&control) == 44);
    assert(!ns6_clock_control_is_locked(&control));
}

static void test_overflow_keeps_the_most_recent_feedback_samples(void) {
    NS6ClockControl control;
    ns6_clock_control_init(&control);
    for (unsigned i = 0; i < NS6_FEEDBACK_PATTERN_QUEUE_CAPACITY + 3u; ++i)
        ns6_clock_control_observe(&control, (i % 2u) ? 44 : 45);

    /* The oldest three reports were discarded; the retained queue starts on
       an even index (44) and remains ordered. */
    for (unsigned i = 3; i < NS6_FEEDBACK_PATTERN_QUEUE_CAPACITY + 3u; ++i) {
        unsigned packets[8];
        ns6_clock_control_next_millisecond(&control, packets);
        assert(sum(packets) == ((i % 2u) ? 44u : 45u));
    }
    assert(ns6_clock_control_sent_total(&control) == 44u * 128u + 45u * 128u);
}

int main(void) {
    test_feedback_selects_the_exact_hopper_pattern();
    test_feedback_reports_keep_order_across_a_usb_output_block();
    test_startup_uses_exact_nominal_pattern_until_feedback_arrives();
    test_invalid_reports_are_ignored_and_reset_discards_pending_patterns();
    test_overflow_keeps_the_most_recent_feedback_samples();
    puts("NS6 Hopper feedback pattern tests passed");
    return 0;
}
