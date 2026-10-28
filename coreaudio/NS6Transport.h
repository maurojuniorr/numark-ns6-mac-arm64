#pragma once
#include <CoreAudio/CoreAudioTypes.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#define NS6_TRANSPORT_RING_FRAMES 16384

typedef struct {
    unsigned char samples[NS6_TRANSPORT_RING_FRAMES * 12];
    _Atomic uint64_t tags[NS6_TRANSPORT_RING_FRAMES];
    _Atomic uint64_t read_cursor;
    _Atomic uint64_t high_water;
    atomic_bool initialized;
} NS6TransportShared;

typedef struct {
    double read_fraction;
} NS6TransportReader;

typedef enum {
    NS6_TRANSPORT_STOP_NONE = 0,
    NS6_TRANSPORT_STOP_EMPTY,
    NS6_TRANSPORT_STOP_MISSING_TAG,
    NS6_TRANSPORT_STOP_RING_LIMIT,
    NS6_TRANSPORT_STOP_INVALID
} NS6TransportStopReason;

void ns6_transport_init_shared(NS6TransportShared *shared);
/* Bind before starting the USB worker; NULL restores the process-local default ring. */
void ns6_transport_bind_shared(NS6TransportShared *shared, NS6TransportReader *reader);
void ns6_transport_reset_shared(NS6TransportShared *shared);
void ns6_transport_discard_shared(NS6TransportShared *shared, NS6TransportReader *reader);
bool ns6_transport_enqueue_shared(NS6TransportShared *shared, const void *frames, UInt32 count,
                                 const AudioStreamBasicDescription *format);
bool ns6_transport_enqueue_at_shared(NS6TransportShared *shared, const void *frames,
                                     UInt64 sample_time, UInt32 count,
                                     const AudioStreamBasicDescription *format);
bool ns6_transport_mix_at_shared(NS6TransportShared *shared, const void *frames,
                                UInt64 sample_time, UInt32 count,
                                const AudioStreamBasicDescription *format);
UInt32 ns6_transport_available_shared(const NS6TransportShared *shared);
UInt32 ns6_transport_dequeue_shared(NS6TransportShared *shared, NS6TransportReader *reader,
                                    void *frames, UInt32 capacity);
UInt32 ns6_transport_dequeue_resampled_shared(NS6TransportShared *shared,
                                              NS6TransportReader *reader, void *frames,
                                              UInt32 output_frames,
                                              double source_frames_per_output_frame);
UInt32 ns6_transport_dequeue_resampled_shared_diagnostic(
    NS6TransportShared *shared, NS6TransportReader *reader, void *frames,
    UInt32 output_frames, double source_frames_per_output_frame,
    NS6TransportStopReason *stop_reason);
UInt32 ns6_transport_advance_shared(NS6TransportShared *shared, UInt32 frames);
UInt32 ns6_transport_advance_resampled_shared(NS6TransportShared *shared,
                                              NS6TransportReader *reader,
                                              UInt32 output_frames,
                                              double source_frames_per_output_frame);

bool ns6_transport_enqueue(const void *frames, UInt32 count, const AudioStreamBasicDescription *format);
bool ns6_transport_enqueue_at(const void *frames, UInt64 sample_time, UInt32 count, const AudioStreamBasicDescription *format);
UInt32 ns6_transport_dequeue(void *frames, UInt32 capacity);
UInt32 ns6_transport_dequeue_resampled(void *frames, UInt32 output_frames,
                                       double source_frames_per_output_frame);
UInt32 ns6_transport_dequeue_resampled_diagnostic(
    void *frames, UInt32 output_frames, double source_frames_per_output_frame,
    NS6TransportStopReason *stop_reason);
UInt32 ns6_transport_available(void);
UInt32 ns6_transport_advance(UInt32 frames);
UInt32 ns6_transport_advance_resampled(UInt32 output_frames,
                                       double source_frames_per_output_frame);
void ns6_transport_reset(void);
void ns6_transport_discard(void);
