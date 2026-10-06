#include "NS6USBRealtime.h"

#include <assert.h>
#include <mach/mach_time.h>
#include <stdint.h>

int main(void) {
    NS6USBRealtimeInterval interval = {0};
    assert(ns6_usb_realtime_interval_init(&interval));

    mach_timebase_info_data_t timebase = {0};
    assert(mach_timebase_info(&timebase) == KERN_SUCCESS);
    assert(timebase.numer != 0 && timebase.denom != 0);

    uint64_t start = mach_absolute_time();
    uint64_t period_ticks = ns6_usb_realtime_duration_ticks(
        4000000, timebase.numer, timebase.denom);
    assert(period_ticks > 0);
    uint64_t deadline = ns6_usb_realtime_deadline(start, period_ticks);
    assert(deadline > start);
    assert(ns6_usb_realtime_interval_begin(&interval, start, deadline) == 0);
    assert(ns6_usb_realtime_interval_finish(&interval) == 0);

    ns6_usb_realtime_interval_destroy(&interval);
    assert(interval.interval == NULL);
    assert(!interval.joined);

    assert(ns6_usb_realtime_duration_ticks(4000000, 125, 3) == 96000);
    assert(ns6_usb_realtime_deadline(UINT64_MAX - 5, 10) == UINT64_MAX);
    return 0;
}
