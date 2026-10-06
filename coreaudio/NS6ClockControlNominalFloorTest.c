#include "NS6ClockControl.h"

#include <assert.h>
#include <stdio.h>

int main(void) {
    NS6ClockControl control;
    ns6_clock_control_init(&control);

    /* Model the low feedback average observed during the audible 3-second
       artifact: 72 of 1024 reports are 45 frames and the rest are 44. */
    for (unsigned i = 0; i < NS6_FEEDBACK_WINDOW; ++i)
        ns6_clock_control_observe(&control, i < 72 ? 45 : 44);

    assert(ns6_clock_control_rate_millihz(&control) < 44100000);
    assert(ns6_clock_control_output_rate_millihz(&control) == 44100000);
    assert(ns6_clock_control_source_frames_per_output_frame(&control, 4096) == 1.0);

    unsigned packets[8];
    uint64_t sent_frames = 0;
    for (unsigned millisecond = 0; millisecond < 1000; ++millisecond) {
        ns6_clock_control_next_millisecond(&control, packets);
        for (unsigned packet = 0; packet < 8; ++packet)
            sent_frames += packets[packet];
    }
    assert(sent_frames == 44100);

    puts("NS6 nominal-floor A/B clock test passed");
    return 0;
}
