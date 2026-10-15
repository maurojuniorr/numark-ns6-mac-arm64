#ifndef NS6_BULK_IN_IDLE_POLICY_H
#define NS6_BULK_IN_IDLE_POLICY_H

#include <stdbool.h>

#ifndef NS6_KEEP_WAVEFORM_DRAIN_IDLE
#define NS6_KEEP_WAVEFORM_DRAIN_IDLE 0
#endif

static inline bool ns6_bulk_in_reads_should_run(
    bool midi_session_active, bool capture_requested) {
    return NS6_KEEP_WAVEFORM_DRAIN_IDLE || !midi_session_active || capture_requested;
}

#endif
