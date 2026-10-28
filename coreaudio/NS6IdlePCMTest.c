#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "NS6IdlePCM.h"

int main(void) {
    uint8_t output[3u * 12u];

    memset(output, 0xa5, sizeof(output));
    ns6_fill_idle_pcm(output, 3u, false);
    for (unsigned i = 0; i < sizeof(output); ++i) {
        assert(output[i] == 0);
    }

    memset(output, 0xa5, sizeof(output));
    ns6_fill_idle_pcm(output, 3u, true);
    for (unsigned i = 0; i < sizeof(output); ++i) {
        bool is_channel_lsb = (i % 12u) == 0u || (i % 12u) == 3u ||
                              (i % 12u) == 6u || (i % 12u) == 9u;
        assert(output[i] == (is_channel_lsb ? 1u : 0u));
    }
    return 0;
}
