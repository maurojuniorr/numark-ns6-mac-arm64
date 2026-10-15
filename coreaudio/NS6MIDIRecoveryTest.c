#include "NS6MIDIRecovery.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    assert(ns6_midi_recovery_error_class(0xe00002ed, 0xe00002ed, 0xe000404f) == NS6_MIDI_ERROR_NOT_RESPONDING);
    assert(ns6_midi_recovery_error_class(0xe000404f, 0xe00002ed, 0xe000404f) == NS6_MIDI_ERROR_PIPE_STALLED);
    assert(ns6_midi_recovery_error_class(0x12345678, 0xe00002ed, 0xe000404f) == NS6_MIDI_ERROR_OTHER);
    assert(ns6_midi_recovery_withhold_not_responding(0xe00002ed, 0xe00002ed, 1));
    assert(!ns6_midi_recovery_withhold_not_responding(0xe00002ed, 0xe00002ed, 0));
    assert(!ns6_midi_recovery_withhold_not_responding(0xe000404f, 0xe00002ed, 1));
    assert(!ns6_midi_recovery_withhold_not_responding(0x12345678, 0xe00002ed, 1));
    assert(ns6_midi_recovery_withhold_not_responding(
        0xe00002ed, 0xe00002ed, NS6_MIDI_DIAGNOSTIC_NO_RETRY_NOT_RESPONDING) ==
        NS6_MIDI_DIAGNOSTIC_NO_RETRY_NOT_RESPONDING);
    assert(ns6_midi_recovery_requires_pipe_clear(0xe000404f, 0xe000404f));
    assert(!ns6_midi_recovery_requires_pipe_clear(0xe00002ed, 0xe000404f));
    assert(!ns6_midi_recovery_requires_pipe_clear(0x12345678, 0xe000404f));
    uint32_t inflight = 0;
    inflight = ns6_midi_in_flight_set(inflight, 0);
    inflight = ns6_midi_in_flight_set(inflight, 1);
    inflight = ns6_midi_in_flight_set(inflight, 3);
    assert(inflight == 11u);
    assert(ns6_midi_in_flight_count(inflight) == 3u);
    inflight = ns6_midi_in_flight_clear(inflight, 1);
    assert(inflight == 9u);
    assert(ns6_midi_in_flight_count(inflight) == 2u);
    assert(ns6_midi_in_flight_clear(inflight, 4) == inflight);
    assert(ns6_midi_in_flight_minimum(4u, 3u) == 3u);
    assert(ns6_midi_in_flight_minimum(2u, 3u) == 2u);
    assert(ns6_midi_recovery_elapsed_us(1000u, 1350u) == 350u);
    assert(ns6_midi_recovery_elapsed_us(1350u, 1000u) == 0u);
    assert(ns6_midi_recovery_log_due(100u, 0u, 5000u));
    assert(!ns6_midi_recovery_log_due(5099u, 100u, 5000u));
    assert(ns6_midi_recovery_log_due(5100u, 100u, 5000u));
    assert(!ns6_midi_recovery_log_due(90u, 100u, 5000u));
    assert(ns6_midi_recovery_retry_backoff_ms(1u, 0u, 1) == 0u);
    assert(ns6_midi_recovery_retry_backoff_ms(2u, 1u, 1) == 50u);
    assert(ns6_midi_recovery_retry_backoff_ms(3u, 0u, 1) == 100u);
    assert(ns6_midi_recovery_retry_backoff_ms(2u, 0u, 0) == 100u);
    uint32_t pending = 0;
    pending = ns6_midi_recovery_add(pending, 0);
    pending = ns6_midi_recovery_add(pending, 2);
    pending = ns6_midi_recovery_add(pending, 2);
    assert(pending == 5u);
    assert(ns6_midi_recovery_take(&pending) == 5u);
    assert(pending == 0u);
    assert(ns6_midi_recovery_add(0, 4) == 0u);
    assert(ns6_midi_recovery_expand_pipe_stall(1u << 2) == 0x0fu);
    assert(ns6_midi_recovery_expand_pipe_stall(0u) == 0u);
    assert(ns6_midi_recovery_backoff_ms(0) == 0u);
    assert(ns6_midi_recovery_backoff_ms(1) == 50u);
    assert(ns6_midi_recovery_backoff_ms(2) == 100u);
    assert(ns6_midi_recovery_backoff_ms(5) == 800u);
    assert(ns6_midi_recovery_backoff_ms(6) == 1000u);
    assert(ns6_midi_recovery_backoff_ms(20) == 1000u);
    puts("NS6 deferred MIDI recovery queue tests passed");
    return 0;
}
