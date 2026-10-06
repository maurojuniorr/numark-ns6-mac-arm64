#ifndef NS6_USB_TIMING_H
#define NS6_USB_TIMING_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    atomic_uint_fast64_t last_ns;
    atomic_uint_fast64_t intervals;
    atomic_uint_fast64_t total_ns;
    atomic_uint_fast64_t min_ns;
    atomic_uint_fast64_t max_ns;
} NS6USBTiming;

typedef struct {
    uint64_t intervals;
    uint64_t total_ns;
    uint64_t min_ns;
    uint64_t max_ns;
} NS6USBTimingSnapshot;

void ns6_usb_timing_init(NS6USBTiming *timing);
void ns6_usb_timing_record(NS6USBTiming *timing, uint64_t timestamp_ns);
void ns6_usb_timing_record_duration(NS6USBTiming *timing, uint64_t duration_ns);
bool ns6_usb_timing_take_snapshot(NS6USBTiming *timing,
                                 NS6USBTimingSnapshot *snapshot);
bool ns6_usb_timing_is_callback_gap(uint64_t interval_ns);

#endif
