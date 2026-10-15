#ifndef NS6_IDLE_PCM_H
#define NS6_IDLE_PCM_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* Fill four-channel 24-bit PCM with silence or a one-LSB idle keepalive. */
static inline void ns6_fill_idle_pcm(uint8_t *output, uint32_t frames, bool keep_alive) {
    if (!output) {
        return;
    }
    memset(output, 0, (size_t)frames * 12u);
    if (!keep_alive) {
        return;
    }
    for (uint32_t frame = 0; frame < frames; ++frame) {
        for (uint32_t channel = 0; channel < 4u; ++channel) {
            output[(size_t)frame * 12u + channel * 3u] = 1u;
        }
    }
}

#endif
