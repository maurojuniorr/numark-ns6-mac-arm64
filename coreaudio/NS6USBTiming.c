#include "NS6USBTiming.h"

#include <limits.h>
#include <string.h>

void ns6_usb_timing_init(NS6USBTiming *timing) {
    if (!timing) return;
    memset(timing, 0, sizeof(*timing));
    atomic_init(&timing->last_ns, 0);
    atomic_init(&timing->intervals, 0);
    atomic_init(&timing->total_ns, 0);
    atomic_init(&timing->min_ns, UINT64_MAX);
    atomic_init(&timing->max_ns, 0);
}

static void update_min(atomic_uint_fast64_t *value, uint64_t candidate) {
    uint_fast64_t previous = atomic_load_explicit(value, memory_order_relaxed);
    while (candidate < previous &&
           !atomic_compare_exchange_weak_explicit(value, &previous, candidate,
                                                   memory_order_relaxed,
                                                   memory_order_relaxed)) {}
}

static void update_max(atomic_uint_fast64_t *value, uint64_t candidate) {
    uint_fast64_t previous = atomic_load_explicit(value, memory_order_relaxed);
    while (candidate > previous &&
           !atomic_compare_exchange_weak_explicit(value, &previous, candidate,
                                                   memory_order_relaxed,
                                                   memory_order_relaxed)) {}
}

void ns6_usb_timing_record(NS6USBTiming *timing, uint64_t timestamp_ns) {
    if (!timing || !timestamp_ns) return;
    uint64_t previous = atomic_exchange_explicit(&timing->last_ns, timestamp_ns,
                                                  memory_order_relaxed);
    if (!previous || timestamp_ns <= previous) return;
    uint64_t interval = timestamp_ns - previous;
    atomic_fetch_add_explicit(&timing->intervals, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&timing->total_ns, interval, memory_order_relaxed);
    update_min(&timing->min_ns, interval);
    update_max(&timing->max_ns, interval);
}

void ns6_usb_timing_record_duration(NS6USBTiming *timing, uint64_t duration_ns) {
    if (!timing) return;
    atomic_fetch_add_explicit(&timing->intervals, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&timing->total_ns, duration_ns, memory_order_relaxed);
    update_min(&timing->min_ns, duration_ns);
    update_max(&timing->max_ns, duration_ns);
}

bool ns6_usb_timing_take_snapshot(NS6USBTiming *timing,
                                 NS6USBTimingSnapshot *snapshot) {
    if (!timing || !snapshot) return false;
    snapshot->intervals = atomic_exchange_explicit(&timing->intervals, 0,
                                                    memory_order_relaxed);
    snapshot->total_ns = atomic_exchange_explicit(&timing->total_ns, 0,
                                                   memory_order_relaxed);
    snapshot->min_ns = atomic_exchange_explicit(&timing->min_ns, UINT64_MAX,
                                                 memory_order_relaxed);
    snapshot->max_ns = atomic_exchange_explicit(&timing->max_ns, 0,
                                                 memory_order_relaxed);
    return snapshot->intervals != 0;
}

bool ns6_usb_timing_is_callback_gap(uint64_t interval_ns) {
    return interval_ns >= 20000000ULL;
}
