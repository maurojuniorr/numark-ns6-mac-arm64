#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t peak[4];
    uint64_t nonzero_frames;
} NS6PCMLevel;

static inline NS6PCMLevel ns6_pcm24_measure(const unsigned char *pcm, unsigned frames) {
    NS6PCMLevel level = {{0, 0, 0, 0}, 0};
    for (unsigned frame = 0; frame < frames; ++frame) {
        int nonzero = 0;
        for (unsigned channel = 0; channel < 4; ++channel) {
            const unsigned char *sample = pcm + ((size_t)frame * 4 + channel) * 3;
            uint32_t raw = (uint32_t)sample[0] | ((uint32_t)sample[1] << 8) |
                           ((uint32_t)sample[2] << 16);
            int32_t value = (int32_t)(raw & 0x7fffff);
            if (raw & 0x800000) value -= 0x800000;
            uint32_t magnitude = value < 0 ? (uint32_t)(-(int64_t)value) : (uint32_t)value;
            if (magnitude > level.peak[channel]) level.peak[channel] = magnitude;
            nonzero |= value != 0;
        }
        level.nonzero_frames += (uint64_t)nonzero;
    }
    return level;
}
