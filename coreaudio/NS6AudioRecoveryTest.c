#include "NS6AudioRecovery.h"

#include <assert.h>
#include <stdio.h>

static void test_healthy_queue_plays_immediately(void) {
    NS6AudioRecovery state;
    ns6_audio_recovery_init(&state);
    assert(!ns6_audio_recovery_needs_silence(&state, 4096));
    assert(ns6_audio_recovery_next_gain(&state) == 1.0);
}

static void test_low_queue_rebuffers_once_before_resuming(void) {
    NS6AudioRecovery state;
    ns6_audio_recovery_init(&state);

    assert(ns6_audio_recovery_needs_silence(&state, 255));
    assert(ns6_audio_recovery_needs_silence(&state, 900));
    assert(!ns6_audio_recovery_needs_silence(&state, 1024));
    assert(!ns6_audio_recovery_needs_silence(&state, 900));
}

static void test_resume_uses_short_monotonic_fade(void) {
    NS6AudioRecovery state;
    ns6_audio_recovery_init(&state);
    assert(ns6_audio_recovery_needs_silence(&state, 1));
    assert(!ns6_audio_recovery_needs_silence(&state, 1024));

    double previous = 0.0;
    for (unsigned frame = 0; frame < NS6_RECOVERY_FADE_FRAMES; ++frame) {
        double gain = ns6_audio_recovery_next_gain(&state);
        assert(gain > previous);
        assert(gain <= 1.0);
        previous = gain;
    }
    assert(previous == 1.0);
    assert(ns6_audio_recovery_next_gain(&state) == 1.0);
}

int main(void) {
    test_healthy_queue_plays_immediately();
    test_low_queue_rebuffers_once_before_resuming();
    test_resume_uses_short_monotonic_fade();
    puts("NS6 audio recovery tests passed");
    return 0;
}
