#include "NS6USBTiming.h"

#include <assert.h>
#include <stdint.h>

int main(void) {
    assert(!ns6_usb_timing_is_callback_gap(19999999ULL));
    assert(ns6_usb_timing_is_callback_gap(20000000ULL));

    NS6USBTiming timing;
    NS6USBTimingSnapshot snapshot;
    ns6_usb_timing_init(&timing);
    ns6_usb_timing_record(&timing, 1000000000ULL);
    ns6_usb_timing_record(&timing, 1004000000ULL);
    ns6_usb_timing_record(&timing, 1009000000ULL);
    assert(ns6_usb_timing_take_snapshot(&timing, &snapshot));
    assert(snapshot.intervals == 2);
    assert(snapshot.total_ns == 9000000ULL);
    assert(snapshot.max_ns == 5000000ULL);
    assert(snapshot.min_ns == 4000000ULL);
    assert(!ns6_usb_timing_take_snapshot(&timing, &snapshot));

    ns6_usb_timing_record_duration(&timing, 2000000ULL);
    ns6_usb_timing_record_duration(&timing, 5000000ULL);
    ns6_usb_timing_record_duration(&timing, 1000000ULL);
    assert(ns6_usb_timing_take_snapshot(&timing, &snapshot));
    assert(snapshot.intervals == 3);
    assert(snapshot.total_ns == 8000000ULL);
    assert(snapshot.min_ns == 1000000ULL);
    assert(snapshot.max_ns == 5000000ULL);
    return 0;
}
