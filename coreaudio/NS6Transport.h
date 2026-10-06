#pragma once
#include <CoreAudio/CoreAudioTypes.h>
#include <stdbool.h>
#define NS6_TRANSPORT_RING_FRAMES 16384
bool ns6_transport_enqueue(const void *frames, UInt32 count, const AudioStreamBasicDescription *format);
bool ns6_transport_enqueue_at(const void *frames, UInt64 sample_time, UInt32 count, const AudioStreamBasicDescription *format);
UInt32 ns6_transport_dequeue(void *frames, UInt32 capacity);
UInt32 ns6_transport_dequeue_resampled(void *frames, UInt32 output_frames,
                                       double source_frames_per_output_frame);
UInt32 ns6_transport_available(void);
UInt32 ns6_transport_advance(UInt32 frames);
UInt32 ns6_transport_advance_resampled(UInt32 output_frames,
                                       double source_frames_per_output_frame);
void ns6_transport_reset(void);
void ns6_transport_discard(void);
