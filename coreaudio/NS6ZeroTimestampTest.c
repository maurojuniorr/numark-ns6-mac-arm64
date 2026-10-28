#include "NS6ZeroTimestamp.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

int main(void) {
    assert(NS6_ZERO_TIMESTAMP_PERIOD >= 10923u);
    assert(NS6_ZERO_TIMESTAMP_PERIOD == 16384u);

    NS6ZeroTimestamp value = ns6_zero_timestamp_calculate(
        1000000u, 1000000u + 2000000000u, 1u, 1u, 44100.0);
    assert(value.sample_time == 81920u);
    assert(value.sample_time % NS6_ZERO_TIMESTAMP_PERIOD == 0u);
    assert(value.host_time >= 1000000u);
    assert(value.host_time <= 1000000u + 2000000000u);

    uint64_t anchor = ns6_zero_timestamp_keep_anchor(0, 1000000u);
    assert(anchor == 1000000u);
    NS6ZeroTimestamp before_restart = ns6_zero_timestamp_calculate(
        anchor, anchor + 1000000000u, 1u, 1u, 44100.0);
    anchor = ns6_zero_timestamp_keep_anchor(anchor, anchor + 1000000000u);
    NS6ZeroTimestamp after_restart = ns6_zero_timestamp_calculate(
        anchor, anchor + 2000000000u, 1u, 1u, 44100.0);
    assert(anchor == 1000000u);
    assert(after_restart.sample_time > before_restart.sample_time);

    puts("NS6 zero timestamp tests passed");
    return 0;
}
