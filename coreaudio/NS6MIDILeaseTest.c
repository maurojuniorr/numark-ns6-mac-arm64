#include "NS6MIDILease.h"
#include <assert.h>

int main(void) {
    assert(ns6_midi_should_poll(true, true));
    assert(!ns6_midi_should_poll(true, false));
    assert(!ns6_midi_should_poll(false, true));
    assert(!ns6_midi_lease_expired(false, 100, 5000, 2000));
    assert(!ns6_midi_lease_expired(true, 1000, 2999, 2000));
    assert(ns6_midi_lease_expired(true, 1000, 3000, 2000));
    assert(!ns6_midi_lease_expired(true, 1000, 999, 2000));
    assert(!ns6_midi_lease_expired(true, 0, 5000, 2000));
    const unsigned char active_message[] = {'N', 'S', '6', 'L', 1};
    const unsigned char inactive_message[] = {'N', 'S', '6', 'L', 0};
    const unsigned char invalid_message[] = {'N', 'S', '6', 'L', 2};
    bool active = false;
    assert(ns6_midi_lease_decode(active_message, sizeof(active_message), &active));
    assert(active);
    assert(ns6_midi_lease_decode(inactive_message, sizeof(inactive_message), &active));
    assert(!active);
    assert(!ns6_midi_lease_decode(invalid_message, sizeof(invalid_message), &active));
    assert(!ns6_midi_lease_decode(active_message, sizeof(active_message) - 1, &active));
    return 0;
}
