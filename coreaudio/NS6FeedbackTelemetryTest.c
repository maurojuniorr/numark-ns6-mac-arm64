#include "NS6FeedbackTelemetry.h"

#include <assert.h>
#include <stdio.h>

static void test_feedback_age_is_reported_for_known_timestamp(void) {
    uint64_t age = 0;
    assert(ns6_feedback_elapsed_ms(1015, 1000, &age));
    assert(age == 15);
}

static void test_feedback_age_is_unknown_without_valid_timestamp(void) {
    uint64_t age = 123;
    assert(!ns6_feedback_elapsed_ms(1015, 0, &age));
    assert(age == 123);
}

static void test_feedback_age_rejects_non_monotonic_interval(void) {
    uint64_t age = 123;
    assert(!ns6_feedback_elapsed_ms(999, 1000, &age));
    assert(age == 123);
}

int main(void) {
    test_feedback_age_is_reported_for_known_timestamp();
    test_feedback_age_is_unknown_without_valid_timestamp();
    test_feedback_age_rejects_non_monotonic_interval();
    puts("NS6 feedback telemetry tests passed");
    return 0;
}
