#ifndef NS6_MIDI_LEASE_H
#define NS6_MIDI_LEASE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NS6_MIDI_LEASE_MESSAGE_BYTES 5
#define NS6_MIDI_LEASE_TIMEOUT_MS 2000

static inline bool ns6_midi_should_poll(bool enabled, bool session_running) {
    return enabled && session_running;
}

static inline bool ns6_midi_lease_expired(bool active, uint64_t last_activity_ms,
                                         uint64_t now_ms, uint64_t timeout_ms) {
    return active && last_activity_ms != 0 && now_ms >= last_activity_ms &&
           now_ms - last_activity_ms >= timeout_ms;
}

static inline bool ns6_midi_lease_decode(const unsigned char *data, size_t length,
                                         bool *active) {
    if (!data || !active || length != NS6_MIDI_LEASE_MESSAGE_BYTES ||
        data[0] != 'N' || data[1] != 'S' || data[2] != '6' || data[3] != 'L' ||
        data[4] > 1) {
        return false;
    }
    *active = data[4] != 0;
    return true;
}

#endif
