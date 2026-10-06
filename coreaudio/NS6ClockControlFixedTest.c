#include "NS6ClockControl.h"

#include <stdio.h>

int main(void) {
    NS6ClockControl control;
    ns6_clock_control_init(&control);
    if (ns6_clock_control_source_frames_per_output_frame(&control, 4096) != 1.0) return 1;
    unsigned long long frames = 0;
    for (unsigned ms = 0; ms < 1000; ++ms) {
        unsigned packet_frames[8];
        ns6_clock_control_next_millisecond(&control, packet_frames);
        for (unsigned packet = 0; packet < 8; ++packet) frames += packet_frames[packet];
    }
    if (frames != 44100) {
        fprintf(stderr, "fixed fallback emitted %llu frames; expected 44100\n", frames);
        return 1;
    }
    puts("NS6 fixed-rate fallback test passed");
    return 0;
}
