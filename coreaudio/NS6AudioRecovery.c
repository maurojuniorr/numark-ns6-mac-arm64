#include "NS6AudioRecovery.h"

void ns6_audio_recovery_init(NS6AudioRecovery *state) {
    if (!state) return;
    state->buffering = false;
    state->fade_frames_remaining = 0;
}

bool ns6_audio_recovery_needs_silence(NS6AudioRecovery *state,
                                      uint32_t available_frames) {
    if (!state) return false;
    if (!state->buffering && available_frames < NS6_RECOVERY_LOW_WATER_FRAMES)
        state->buffering = true;
    if (!state->buffering) return false;
    if (available_frames < NS6_RECOVERY_RESUME_FRAMES) return true;
    state->buffering = false;
    state->fade_frames_remaining = NS6_RECOVERY_FADE_FRAMES;
    return false;
}

double ns6_audio_recovery_next_gain(NS6AudioRecovery *state) {
    if (!state || state->fade_frames_remaining == 0) return 1.0;
    uint32_t completed = NS6_RECOVERY_FADE_FRAMES - state->fade_frames_remaining + 1;
    --state->fade_frames_remaining;
    return (double)completed / (double)NS6_RECOVERY_FADE_FRAMES;
}
