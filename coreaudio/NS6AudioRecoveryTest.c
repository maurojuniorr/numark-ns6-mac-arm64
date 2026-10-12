#include "NS6AudioRecovery.h"

#include <assert.h>
#include <stdio.h>

static void test_missing_audio_tail_fades_to_zero(void) {
    const uint32_t missing_frames = 6;
    double previous_gain = 1.0;
    for (uint32_t frame = 0; frame < missing_frames; ++frame) {
        double gain = ns6_audio_recovery_underrun_gain(frame, missing_frames);
        assert(gain < previous_gain);
        assert(gain >= 0.0);
        previous_gain = gain;
    }
    assert(previous_gain == 0.0);
}

static void test_full_packet_loss_fades_the_last_sample_to_zero(void) {
    const uint32_t missing_frames = 44;
    assert(ns6_audio_recovery_underrun_gain(0, missing_frames) > 0.9);
    assert(ns6_audio_recovery_underrun_gain(missing_frames - 1, missing_frames) == 0.0);
}

int main(void) {
    test_missing_audio_tail_fades_to_zero();
    test_full_packet_loss_fades_the_last_sample_to_zero();
    puts("NS6 audio recovery tests passed");
    return 0;
}
