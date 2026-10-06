#include "NS6MIDIRecovery.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    uint32_t pending = 0;
    pending = ns6_midi_recovery_add(pending, 0);
    pending = ns6_midi_recovery_add(pending, 2);
    pending = ns6_midi_recovery_add(pending, 2);
    assert(pending == 5u);
    assert(ns6_midi_recovery_take(&pending) == 5u);
    assert(pending == 0u);
    assert(ns6_midi_recovery_add(0, 4) == 0u);
    assert(ns6_midi_recovery_backoff_ms(0) == 0u);
    assert(ns6_midi_recovery_backoff_ms(1) == 50u);
    assert(ns6_midi_recovery_backoff_ms(2) == 100u);
    assert(ns6_midi_recovery_backoff_ms(5) == 800u);
    assert(ns6_midi_recovery_backoff_ms(6) == 1000u);
    assert(ns6_midi_recovery_backoff_ms(20) == 1000u);
    puts("NS6 deferred MIDI recovery queue tests passed");
    return 0;
}
