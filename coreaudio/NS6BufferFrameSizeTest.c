#include "NS6BufferFrameSize.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    assert(ns6_buffer_frame_size_request_valid(49, 49, 1024));
    assert(ns6_buffer_frame_size_request_valid(96, 49, 1024));
    assert(ns6_buffer_frame_size_request_valid(1024, 49, 1024));
    assert(!ns6_buffer_frame_size_request_valid(0, 49, 1024));
    assert(!ns6_buffer_frame_size_request_valid(48, 49, 1024));
    assert(!ns6_buffer_frame_size_request_valid(1025, 49, 1024));
    puts("NS6 CoreAudio buffer frame-size request tests passed");
    return 0;
}
