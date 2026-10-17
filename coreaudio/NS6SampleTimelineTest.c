#include "NS6SampleTimeline.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

int main(void) {
    uint64_t sample_time = 0;
    assert(ns6_sample_time_from_nanos(1000000000ull, 44100, &sample_time));
    assert(sample_time == 44100);
    assert(ns6_sample_time_from_nanos(2500000000ull, 44100, &sample_time));
    assert(sample_time == 110250);
    assert(ns6_sample_time_from_nanos(2500000000ull, 44100, &sample_time));
    assert(sample_time == 110250);
    assert(!ns6_sample_time_from_nanos(1, 0, &sample_time));
    assert(!ns6_sample_time_from_nanos(1, 44100, NULL));
    assert(!ns6_sample_time_from_nanos(UINT64_MAX, UINT32_MAX, &sample_time));
    puts("NS6 host-time sample timeline tests passed");
    return 0;
}
