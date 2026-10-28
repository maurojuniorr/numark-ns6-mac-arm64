#ifndef NS6_MIDI_READ_LIFECYCLE_H
#define NS6_MIDI_READ_LIFECYCLE_H

#include <stdbool.h>

#ifndef NS6_MIDI_KEEP_INPUT_IDLE
#define NS6_MIDI_KEEP_INPUT_IDLE 0
#endif

#ifndef NS6_MIDI_IDLE_READ_COUNT
#define NS6_MIDI_IDLE_READ_COUNT (NS6_MIDI_KEEP_INPUT_IDLE ? 4 : 0)
#endif

#ifndef NS6_MIDI_INITIAL_READ_COUNT
#define NS6_MIDI_INITIAL_READ_COUNT 4
#endif

#define NS6_MIDI_ACTIVE_READ_COUNT 4


enum NS6MIDIReadAction {
    NS6_MIDI_READ_NOOP,
    NS6_MIDI_READ_QUIESCE,
    NS6_MIDI_READ_REARM
};

static inline enum NS6MIDIReadAction ns6_midi_read_action(
    bool session_running, unsigned target_count, unsigned active_count) {
    if (!session_running || target_count == active_count)
        return NS6_MIDI_READ_NOOP;
    return target_count > active_count ? NS6_MIDI_READ_REARM : NS6_MIDI_READ_QUIESCE;
}

static inline unsigned ns6_midi_read_target_count(bool session_running, bool audio_active) {
    if (!session_running)
        return 0;
    unsigned count=audio_active?NS6_MIDI_ACTIVE_READ_COUNT:NS6_MIDI_IDLE_READ_COUNT;
    return count>NS6_MIDI_ACTIVE_READ_COUNT?NS6_MIDI_ACTIVE_READ_COUNT:count;
}

static inline unsigned ns6_midi_read_initial_count(bool session_running) {
    if(!session_running)return 0;
    return NS6_MIDI_INITIAL_READ_COUNT>NS6_MIDI_ACTIVE_READ_COUNT
        ? NS6_MIDI_ACTIVE_READ_COUNT:NS6_MIDI_INITIAL_READ_COUNT;
}

static inline unsigned ns6_midi_read_target_mask(unsigned target_count) {
    return target_count >= NS6_MIDI_ACTIVE_READ_COUNT
        ? (1u << NS6_MIDI_ACTIVE_READ_COUNT) - 1u
        : target_count ? (1u << target_count) - 1u : 0u;
}

static inline unsigned ns6_midi_read_missing_mask(unsigned target_count, unsigned in_flight_mask) {
    return ns6_midi_read_target_mask(target_count) & ~in_flight_mask;
}

static inline bool ns6_midi_read_should_submit(
    bool session_running, unsigned target_count, unsigned slot, bool recovery_active) {
    return session_running && slot < target_count && !recovery_active;
}

static inline bool ns6_midi_read_completion_should_continue(
    bool session_running, unsigned target_count, unsigned slot, bool recovery_active) {
    return ns6_midi_read_should_submit(session_running, target_count, slot, recovery_active);
}

#endif
