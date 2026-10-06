#include "NS6USBFrameTimestamps.h"

#include <assert.h>

int main(void) {
    IOUSBLowLatencyIsocFrame frames[5] = {0};
    NS6USBFrameTimestampRange range =
        ns6_usb_frame_timestamp_range(frames, 5);
    assert(range.valid_frames == 0);

    frames[0].frTimeStamp.lo = 900;
    frames[1].frTimeStamp.lo = 0;
    frames[2].frTimeStamp.lo = 1200;
    frames[3].frTimeStamp.lo = 1000;
    frames[4].frTimeStamp.lo = 1500;
    range = ns6_usb_frame_timestamp_range(frames, 5);
    assert(range.valid_frames == 4);
    assert(range.oldest_ticks == 900);
    assert(range.newest_ticks == 1500);
    assert(ns6_usb_frame_timestamp_range(NULL, 5).valid_frames == 0);
    return 0;
}
