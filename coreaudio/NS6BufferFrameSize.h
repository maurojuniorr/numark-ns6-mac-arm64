#ifndef NS6_BUFFER_FRAME_SIZE_H
#define NS6_BUFFER_FRAME_SIZE_H

#include <stdbool.h>
#include <stdint.h>

static inline bool ns6_buffer_frame_size_request_valid(uint32_t frames,
                                                       uint32_t minimum,
                                                       uint32_t maximum) {
    return frames != 0 && minimum != 0 && minimum <= maximum &&
           frames >= minimum && frames <= maximum;
}

#endif
