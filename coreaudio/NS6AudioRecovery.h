#ifndef NS6_AUDIO_RECOVERY_H
#define NS6_AUDIO_RECOVERY_H

#include <stdint.h>

/* Fade only the samples missing from a genuinely short USB output block. */
double ns6_audio_recovery_underrun_gain(uint32_t frame_index,
                                        uint32_t missing_frames);

#endif
