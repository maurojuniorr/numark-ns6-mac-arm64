#include "NS6PCMLevel.h"
#include <assert.h>

int main(void) {
    unsigned char samples[2 * 4 * 3] = {0};
    NS6PCMLevel level = ns6_pcm24_measure(samples, 2);
    assert(level.nonzero_frames == 0);
    for (unsigned channel = 0; channel < 4; ++channel) assert(level.peak[channel] == 0);

    samples[0] = 0xff; samples[1] = 0xff; samples[2] = 0x7f;
    samples[3] = 0x00; samples[4] = 0x00; samples[5] = 0x80;
    samples[(4 + 2) * 3 + 2] = 0x01;
    level = ns6_pcm24_measure(samples, 2);
    assert(level.peak[0] == 8388607);
    assert(level.peak[1] == 8388608);
    assert(level.peak[2] == 65536);
    assert(level.peak[3] == 0);
    assert(level.nonzero_frames == 2);
    return 0;
}
