#include "NS6MIDIReadLifecycle.h"

#include <assert.h>

int main(void) {
    assert(ns6_midi_read_initial_count(false) == 0u);
    assert(ns6_midi_read_initial_count(true) == NS6_MIDI_INITIAL_READ_COUNT);
    assert(ns6_midi_read_target_count(false, false) == 0u);
    assert(ns6_midi_read_target_count(true, true) == 4u);
    assert(ns6_midi_read_action(true, 1u, 1u) == NS6_MIDI_READ_NOOP);
    assert(ns6_midi_read_action(true, 4u, 1u) == NS6_MIDI_READ_REARM);
    assert(ns6_midi_read_action(true, 1u, 4u) == NS6_MIDI_READ_QUIESCE);
    assert(ns6_midi_read_action(false, 0u, 1u) == NS6_MIDI_READ_NOOP);
    assert(ns6_midi_read_target_mask(0u) == 0u);
    assert(ns6_midi_read_target_mask(1u) == 1u);
    assert(ns6_midi_read_target_mask(4u) == 0xfu);
    assert(ns6_midi_read_missing_mask(1u, 1u) == 0u);
    assert(ns6_midi_read_missing_mask(4u, 1u) == 0xeu);
    assert(ns6_midi_read_missing_mask(4u, 0xfu) == 0u);
    assert(!ns6_midi_read_should_submit(true, 1u, 1u, false));
    assert(ns6_midi_read_should_submit(true, 1u, 0u, false));
    assert(!ns6_midi_read_should_submit(true, 4u, 2u, true));
    assert(ns6_midi_read_should_submit(true, 4u, 3u, false));
    assert(!ns6_midi_read_should_submit(false, 4u, 0u, false));
    assert(ns6_midi_read_completion_should_continue(true, 1u, 0u, false));
    assert(!ns6_midi_read_completion_should_continue(true, 1u, 1u, false));
    assert(!ns6_midi_read_completion_should_continue(true, 4u, 0u, true));
    assert(!ns6_midi_read_completion_should_continue(false, 4u, 0u, false));
#if NS6_MIDI_KEEP_INPUT_IDLE
    assert(ns6_midi_read_target_count(true, false) == 4u);
#elif NS6_MIDI_IDLE_READ_COUNT
    assert(ns6_midi_read_target_count(true, false) == 1u);
#else
    assert(ns6_midi_read_target_count(true, false) == 0u);
#endif
    return 0;
}
