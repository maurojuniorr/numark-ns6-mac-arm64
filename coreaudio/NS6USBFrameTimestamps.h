#ifndef NS6_USB_FRAME_TIMESTAMPS_H
#define NS6_USB_FRAME_TIMESTAMPS_H

#include <IOKit/usb/IOUSBLib.h>
#include <stdint.h>

typedef struct {
    uint32_t valid_frames;
    uint64_t oldest_ticks;
    uint64_t newest_ticks;
} NS6USBFrameTimestampRange;

static inline int
ns6_usb_frame_timestamp_callback_lag_ticks(NS6USBFrameTimestampRange range,
                                            uint64_t callback_ticks,
                                            uint64_t *lag_ticks) {
    if (!lag_ticks || range.valid_frames == 0 || callback_ticks < range.newest_ticks)
        return 0;
    *lag_ticks = callback_ticks - range.newest_ticks;
    return 1;
}

static inline NS6USBFrameTimestampRange
ns6_usb_frame_timestamp_range(const IOUSBLowLatencyIsocFrame *frames,
                              uint32_t count) {
    NS6USBFrameTimestampRange range = {0};
    if (!frames) return range;

    for (uint32_t i = 0; i < count; ++i) {
        uint64_t timestamp = ((uint64_t)frames[i].frTimeStamp.hi << 32) |
                             frames[i].frTimeStamp.lo;
        if (timestamp == 0) continue;
        if (range.valid_frames == 0 || timestamp < range.oldest_ticks)
            range.oldest_ticks = timestamp;
        if (range.valid_frames == 0 || timestamp > range.newest_ticks)
            range.newest_ticks = timestamp;
        ++range.valid_frames;
    }
    return range;
}

#endif
