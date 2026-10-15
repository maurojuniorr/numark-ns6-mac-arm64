#ifndef NS6_AUDIO_PREFILL_H
#define NS6_AUDIO_PREFILL_H

#include <stdint.h>

static inline uint32_t ns6_audio_startup_prefill_frames(uint32_t configured_frames,
                                                        uint32_t minimum_frames,
                                                        uint32_t queue_target_frames){
    uint32_t threshold=configured_frames;
    if(threshold<minimum_frames)threshold=minimum_frames;
    if(threshold<queue_target_frames)threshold=queue_target_frames;
    return threshold;
}

#endif
