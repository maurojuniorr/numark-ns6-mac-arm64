#ifndef NS6_AUDIO_RECOVERY_H
#define NS6_AUDIO_RECOVERY_H

#include <stdbool.h>
#include <stdint.h>

#define NS6_RECOVERY_LOW_WATER_FRAMES 256u
#define NS6_RECOVERY_RESUME_FRAMES 1024u
#define NS6_RECOVERY_FADE_FRAMES 64u

typedef struct {
    bool buffering;
    uint32_t fade_frames_remaining;
} NS6AudioRecovery;

void ns6_audio_recovery_init(NS6AudioRecovery *state);
bool ns6_audio_recovery_needs_silence(NS6AudioRecovery *state,
                                      uint32_t available_frames);
double ns6_audio_recovery_next_gain(NS6AudioRecovery *state);

#endif
