#pragma once

#include <stdbool.h>
#include <stdint.h>

static inline bool ns6_sample_time_from_nanos(uint64_t nanoseconds, uint32_t sample_rate,
                                               uint64_t *sample_time) {
    if (!sample_time || sample_rate == 0) return false;
    const uint64_t nanos_per_second = 1000000000ull;
    uint64_t seconds = nanoseconds / nanos_per_second;
    uint64_t remainder = nanoseconds % nanos_per_second;
    uint64_t fractional_frames = remainder * sample_rate / nanos_per_second;
    if (seconds > (UINT64_MAX - fractional_frames) / sample_rate) return false;
    *sample_time = seconds * sample_rate + fractional_frames;
    return true;
}
