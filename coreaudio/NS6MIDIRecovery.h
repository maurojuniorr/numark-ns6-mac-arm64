#ifndef NS6_MIDI_RECOVERY_H
#define NS6_MIDI_RECOVERY_H

#include <stdint.h>

#ifndef NS6_MIDI_DIAGNOSTIC_NO_RETRY_NOT_RESPONDING
#define NS6_MIDI_DIAGNOSTIC_NO_RETRY_NOT_RESPONDING 0
#endif

#define NS6_MIDI_RECOVERY_SLOTS 4u
#endif
_Static_assert(NS6_MIDI_RECOVERY_SLOTS > 0 && NS6_MIDI_RECOVERY_SLOTS <= 32,
               "MIDI recovery mask supports 1 to 32 slots");

enum ns6_midi_error_class {
    NS6_MIDI_ERROR_OTHER = 0,
    NS6_MIDI_ERROR_NOT_RESPONDING = 1,
    NS6_MIDI_ERROR_PIPE_STALLED = 2
};

static inline enum ns6_midi_error_class ns6_midi_recovery_error_class(
    uint32_t status, uint32_t not_responding_status, uint32_t pipe_stalled_status) {
    if (status == not_responding_status) return NS6_MIDI_ERROR_NOT_RESPONDING;
    if (status == pipe_stalled_status) return NS6_MIDI_ERROR_PIPE_STALLED;
    return NS6_MIDI_ERROR_OTHER;
}

static inline int ns6_midi_recovery_withhold_not_responding(
    uint32_t status, uint32_t not_responding_status, int diagnostic_enabled) {
    return diagnostic_enabled && status == not_responding_status;
}

static inline int ns6_midi_recovery_requires_pipe_clear(uint32_t status, uint32_t pipe_stalled_status) {
    return status == pipe_stalled_status;
}

static inline int ns6_midi_recovery_should_reidentify(
    int pipe_stall_recovered, int recovery_succeeded, int all_reads_resubmitted) {
    return pipe_stall_recovered && recovery_succeeded && all_reads_resubmitted;
}

static inline uint32_t ns6_midi_in_flight_set(uint32_t mask, unsigned slot) {
    return slot < NS6_MIDI_RECOVERY_SLOTS ? mask | (UINT32_C(1) << slot) : mask;
}

static inline uint32_t ns6_midi_in_flight_clear(uint32_t mask, unsigned slot) {
    return slot < NS6_MIDI_RECOVERY_SLOTS ? mask & ~(UINT32_C(1) << slot) : mask;
}

static inline unsigned ns6_midi_in_flight_count(uint32_t mask) {
    unsigned count = 0;
    while (mask) {
        mask &= mask - 1;
        ++count;
    }
    return count;
}

static inline unsigned ns6_midi_in_flight_minimum(unsigned current, unsigned observed) {
    return observed < current ? observed : current;
}

static inline uint64_t ns6_midi_recovery_elapsed_us(uint64_t started_us, uint64_t finished_us) {
    return finished_us >= started_us ? finished_us - started_us : 0;
}

static inline int ns6_midi_recovery_log_due(uint64_t now_ms, uint64_t last_log_ms, uint64_t interval_ms) {
    return last_log_ms == 0 || (now_ms >= last_log_ms && now_ms - last_log_ms >= interval_ms);
}

static inline uint32_t ns6_midi_recovery_add(uint32_t pending, unsigned slot) {
    return slot < NS6_MIDI_RECOVERY_SLOTS ? pending | (UINT32_C(1) << slot) : pending;
}

static inline uint32_t ns6_midi_recovery_expand_pipe_stall(uint32_t pending) {
    return pending ? (UINT32_C(1) << NS6_MIDI_RECOVERY_SLOTS) - 1 : 0;
}

static inline uint32_t ns6_midi_recovery_take(uint32_t *pending) {
    if (!pending) return 0;
    uint32_t ready = *pending;
    *pending = 0;
    return ready;
}

static inline uint32_t ns6_midi_recovery_ready(uint32_t completed, uint32_t in_flight) {
    return completed & ~in_flight;
}

static inline uint32_t ns6_midi_recovery_backoff_ms(unsigned attempt) {
    if (attempt == 0) return 0;
    if (attempt >= 6) return 1000;
    return 50u << (attempt - 1);
}

static inline uint32_t ns6_midi_recovery_retry_backoff_ms(
    unsigned consecutive_recovery_failures,
    unsigned consecutive_clear_failures,
    int clear_pending) {
    unsigned attempt = consecutive_recovery_failures;
    if (clear_pending) {
        unsigned submission_retry = consecutive_recovery_failures > 0
            ? consecutive_recovery_failures - 1 : 0;
        attempt = consecutive_clear_failures > submission_retry
            ? consecutive_clear_failures : submission_retry;
    }
    return ns6_midi_recovery_backoff_ms(attempt);
}

#endif
