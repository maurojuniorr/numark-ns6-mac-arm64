#include "NS6AudioRecovery.h"

double ns6_audio_recovery_underrun_gain(uint32_t frame_index,
                                        uint32_t missing_frames) {
    if (missing_frames == 0 || frame_index >= missing_frames) return 0.0;
    return (double)(missing_frames - frame_index - 1) / (double)missing_frames;
}
