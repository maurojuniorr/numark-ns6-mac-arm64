#pragma once

#include <stdint.h>

/* AudioServerPlugIn.h requires at least 10,923 frames between zero timestamps.
   Keep this clock period independent from the application's I/O buffer size. */
#define NS6_ZERO_TIMESTAMP_PERIOD 16384u

typedef struct {
    uint64_t sample_time;
    uint64_t host_time;
} NS6ZeroTimestamp;

static inline NS6ZeroTimestamp ns6_zero_timestamp_calculate(
        uint64_t anchor_host_time,
        uint64_t current_host_time,
        uint32_t timebase_numer,
        uint32_t timebase_denom,
        double sample_rate) {
    NS6ZeroTimestamp result = {0u, anchor_host_time};
    if (current_host_time < anchor_host_time || !timebase_numer ||
        !timebase_denom || sample_rate <= 0.0) return result;

    long double elapsed_ticks = (long double)(current_host_time - anchor_host_time);
    long double elapsed_ns = elapsed_ticks * (long double)timebase_numer /
                             (long double)timebase_denom;
    uint64_t frame = (uint64_t)(elapsed_ns * (long double)sample_rate / 1000000000.0L);
    frame -= frame % NS6_ZERO_TIMESTAMP_PERIOD;

    long double host_delta = (long double)frame * 1000000000.0L /
                             (long double)sample_rate *
                             (long double)timebase_denom /
                             (long double)timebase_numer;
    result.sample_time = frame;
    result.host_time = anchor_host_time + (uint64_t)host_delta;
    return result;
}
