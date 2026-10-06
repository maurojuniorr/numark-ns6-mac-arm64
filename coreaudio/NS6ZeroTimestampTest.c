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

    puts("NS6 zero timestamp tests passed");
    return 0;
}
