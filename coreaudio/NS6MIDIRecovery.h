#ifndef NS6_MIDI_RECOVERY_H
#define NS6_MIDI_RECOVERY_H

#include <stdint.h>

#define NS6_MIDI_RECOVERY_SLOTS 4u

static inline uint32_t ns6_midi_recovery_add(uint32_t pending, unsigned slot) {
    return slot < NS6_MIDI_RECOVERY_SLOTS ? pending | (UINT32_C(1) << slot) : pending;
}

static inline uint32_t ns6_midi_recovery_take(uint32_t *pending) {
    if (!pending) return 0;
    uint32_t ready = *pending;
    *pending = 0;
    return ready;
}

static inline uint32_t ns6_midi_recovery_backoff_ms(unsigned attempt) {
    if (attempt == 0) return 0;
    if (attempt >= 6) return 1000;
    return 50u << (attempt - 1);
}

#endif
