#include "NS6Transport.h"
#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

#define CHANNELS 4
#define USB_FRAME_BYTES 12

/* Compatibility state for the existing in-process HAL/USB path. */
static NS6TransportShared default_shared;
static NS6TransportReader default_reader;
static NS6TransportShared *active_shared = &default_shared;
static NS6TransportReader *active_reader = &default_reader;
static atomic_flag output_mix_lock = ATOMIC_FLAG_INIT;

static void pack24(unsigned char *out, int32_t sample) {
    out[0] = (unsigned char)sample;
    out[1] = (unsigned char)(sample >> 8);
    out[2] = (unsigned char)(sample >> 16);
}

static int32_t unpack24(const unsigned char *sample) {
    return (int32_t)sample[0] | ((int32_t)sample[1] << 8) |
           ((int32_t)(int8_t)sample[2] << 16);
}

static int32_t saturate24(int64_t value) {
    if (value > 8388607) return 8388607;
    if (value < -8388608) return -8388608;
    return (int32_t)value;
}

static void mix_lock(void) {
    while (atomic_flag_test_and_set_explicit(&output_mix_lock, memory_order_acquire)) {}
}

static void mix_unlock(void) {
    atomic_flag_clear_explicit(&output_mix_lock, memory_order_release);
}

static bool valid_format(const AudioStreamBasicDescription *format) {
    if (!format || format->mChannelsPerFrame != CHANNELS || !format->mBytesPerFrame ||
        format->mBytesPerFrame % CHANNELS != 0) return false;
    unsigned bytes = format->mBytesPerFrame / CHANNELS;
    if (format->mFormatFlags & kAudioFormatFlagIsFloat)
        return format->mBitsPerChannel == 32 && bytes == sizeof(float);
    return (format->mBitsPerChannel == 24 && bytes == 3) ||
           (format->mBitsPerChannel == 32 && bytes == 4);
}

static void convert_frame(const unsigned char *source, unsigned sample_bytes,
                          bool is_float, UInt32 bits, unsigned char *destination) {
    int32_t samples[CHANNELS];
    for (unsigned channel = 0; channel < CHANNELS; ++channel) {
        const unsigned char *sample_source = source + channel * sample_bytes;
        int32_t sample;
        if (is_float) {
            float value;
            memcpy(&value, sample_source, sizeof(value));
            if (!isfinite(value)) value = 0.0f;
            if (value > 1.0f) value = 1.0f;
            if (value < -1.0f) value = -1.0f;
            sample = (int32_t)lrintf(value * 8388607.0f);
        } else if (bits == 24) {
            sample = (int32_t)sample_source[0] | ((int32_t)sample_source[1] << 8) |
                     ((int32_t)(int8_t)sample_source[2] << 16);
        } else {
            int32_t value;
            memcpy(&value, sample_source, sizeof(value));
            sample = value >> 8;
        }
        samples[channel] = sample;
    }
    for (unsigned channel = 0; channel < CHANNELS; ++channel)
        pack24(destination + channel * 3, samples[channel]);
}

void ns6_transport_init_shared(NS6TransportShared *shared) {
    if (!shared) return;
    for (UInt32 i = 0; i < NS6_TRANSPORT_RING_FRAMES; ++i)
        atomic_init(&shared->tags[i], UINT64_MAX);
    atomic_init(&shared->read_cursor, 0);
    atomic_init(&shared->high_water, 0);
    atomic_init(&shared->initialized, false);
}

void ns6_transport_bind_shared(NS6TransportShared *shared, NS6TransportReader *reader) {
    if (shared && reader) {
        active_shared = shared;
        active_reader = reader;
    } else {
        active_shared = &default_shared;
        active_reader = &default_reader;
    }
}

void ns6_transport_reset_shared(NS6TransportShared *shared) {
    if (!shared) return;
    for (UInt32 i = 0; i < NS6_TRANSPORT_RING_FRAMES; ++i)
        atomic_store_explicit(&shared->tags[i], UINT64_MAX, memory_order_relaxed);
    atomic_store_explicit(&shared->read_cursor, 0, memory_order_relaxed);
    atomic_store_explicit(&shared->high_water, 0, memory_order_relaxed);
    atomic_store_explicit(&shared->initialized, false, memory_order_release);
}

void ns6_transport_discard_shared(NS6TransportShared *shared, NS6TransportReader *reader) {
    if (!shared) return;
    atomic_store_explicit(&shared->initialized, false, memory_order_release);
    for (UInt32 i = 0; i < NS6_TRANSPORT_RING_FRAMES; ++i)
        atomic_store_explicit(&shared->tags[i], UINT64_MAX, memory_order_relaxed);
    atomic_store_explicit(&shared->read_cursor, 0, memory_order_relaxed);
    atomic_store_explicit(&shared->high_water, 0, memory_order_relaxed);
    if (reader) reader->read_fraction = 0.0;
}

bool ns6_transport_enqueue_at_shared(NS6TransportShared *shared, const void *input,
                                     UInt64 sample_time, UInt32 count,
                                     const AudioStreamBasicDescription *format) {
    if (!shared || (!input && count) || !valid_format(format)) return false;
    if (count == 0) return true;
    /* CoreAudio's timeline is independent of the USB FIFO; append completed blocks. */
    sample_time = atomic_load_explicit(&shared->high_water, memory_order_acquire);
    if (sample_time > UINT64_MAX - count) return false;

    if (!atomic_load_explicit(&shared->initialized, memory_order_acquire)) {
        atomic_store_explicit(&shared->read_cursor, sample_time, memory_order_relaxed);
        atomic_store_explicit(&shared->high_water, sample_time, memory_order_relaxed);
        atomic_store_explicit(&shared->initialized, true, memory_order_release);
    }

    uint64_t cursor = atomic_load_explicit(&shared->read_cursor, memory_order_acquire);
    if (sample_time < cursor) {
        sample_time = cursor;
        atomic_store_explicit(&shared->high_water, cursor, memory_order_release);
    }
    uint64_t end = sample_time + count;
    if (end <= cursor) return true;
    UInt32 skip = sample_time < cursor ? (UInt32)(cursor - sample_time) : 0;
    sample_time += skip;
    count -= skip;
    if (count == 0 || sample_time - cursor > NS6_TRANSPORT_RING_FRAMES ||
        count > NS6_TRANSPORT_RING_FRAMES - (sample_time - cursor)) return false;

    unsigned sample_bytes = format->mBytesPerFrame / CHANNELS;
    bool is_float = (format->mFormatFlags & kAudioFormatFlagIsFloat) != 0;
    const unsigned char *source = (const unsigned char *)input + (size_t)skip * format->mBytesPerFrame;
    for (UInt32 frame = 0; frame < count; ++frame) {
        UInt64 position = sample_time + frame;
        UInt32 slot = (UInt32)(position % NS6_TRANSPORT_RING_FRAMES);
        unsigned char *destination = shared->samples + (size_t)slot * USB_FRAME_BYTES;
        convert_frame(source + (size_t)frame * format->mBytesPerFrame, sample_bytes,
                      is_float, format->mBitsPerChannel, destination);
        atomic_store_explicit(&shared->tags[slot], position, memory_order_release);
    }
    uint64_t previous = atomic_load_explicit(&shared->high_water, memory_order_relaxed);
    while (end > previous && !atomic_compare_exchange_weak_explicit(
               &shared->high_water, &previous, end, memory_order_release, memory_order_relaxed)) {}
    return true;
}

bool ns6_transport_enqueue_shared(NS6TransportShared *shared, const void *frames, UInt32 count,
                                 const AudioStreamBasicDescription *format) {
    if (!shared) return false;
    UInt64 position = atomic_load_explicit(&shared->high_water, memory_order_acquire);
    if (!atomic_load_explicit(&shared->initialized, memory_order_acquire)) position = 0;
    return ns6_transport_enqueue_at_shared(shared, frames, position, count, format);
}

bool ns6_transport_mix_at_shared(NS6TransportShared *shared, const void *input,
                                 UInt64 sample_time, UInt32 count,
                                 const AudioStreamBasicDescription *format) {
    if (!shared || (!input && count) || !valid_format(format)) return false;
    if (!count) return true;
    if (sample_time > UINT64_MAX - count) return false;

    const unsigned sample_bytes = format->mBytesPerFrame / CHANNELS;
    const bool is_float = (format->mFormatFlags & kAudioFormatFlagIsFloat) != 0;
    const unsigned char *source = input;
    unsigned char converted[USB_FRAME_BYTES];

    mix_lock();
    if (!atomic_load_explicit(&shared->initialized, memory_order_relaxed)) {
        atomic_store_explicit(&shared->read_cursor, sample_time, memory_order_relaxed);
        atomic_store_explicit(&shared->high_water, sample_time, memory_order_relaxed);
        atomic_store_explicit(&shared->initialized, true, memory_order_release);
    }
    const uint64_t cursor = atomic_load_explicit(&shared->read_cursor, memory_order_acquire);
    if ((sample_time > cursor ? sample_time - cursor : cursor - sample_time) >=
            NS6_TRANSPORT_RING_FRAMES &&
        ns6_transport_available_shared(shared) == 0) {
        atomic_store_explicit(&shared->read_cursor, sample_time, memory_order_release);
        atomic_store_explicit(&shared->high_water, sample_time, memory_order_release);
    }
    const uint64_t aligned_cursor = atomic_load_explicit(&shared->read_cursor, memory_order_acquire);
    if (sample_time < aligned_cursor) {
        const uint64_t late = aligned_cursor - sample_time;
        if (late >= count) {
            mix_unlock();
            return true;
        }
        source += (size_t)late * format->mBytesPerFrame;
        count -= (UInt32)late;
        sample_time = aligned_cursor;
    }
    if (sample_time - aligned_cursor >= NS6_TRANSPORT_RING_FRAMES ||
        count > NS6_TRANSPORT_RING_FRAMES - (sample_time - aligned_cursor)) {
        mix_unlock();
        return false;
    }

    for (UInt32 frame = 0; frame < count; ++frame) {
        const uint64_t position = sample_time + frame;
        const UInt32 slot = (UInt32)(position % NS6_TRANSPORT_RING_FRAMES);
        unsigned char *destination = shared->samples + (size_t)slot * USB_FRAME_BYTES;
        convert_frame(source + (size_t)frame * format->mBytesPerFrame, sample_bytes,
                      is_float, format->mBitsPerChannel, converted);
        if (atomic_load_explicit(&shared->tags[slot], memory_order_relaxed) == position) {
            for (unsigned channel = 0; channel < CHANNELS; ++channel) {
                int64_t sum = (int64_t)unpack24(destination + channel * 3) +
                              unpack24(converted + channel * 3);
                pack24(destination + channel * 3, saturate24(sum));
            }
        } else {
            memcpy(destination, converted, USB_FRAME_BYTES);
            atomic_store_explicit(&shared->tags[slot], position, memory_order_release);
        }
    }
    const uint64_t end = sample_time + count;
    const uint64_t previous = atomic_load_explicit(&shared->high_water, memory_order_relaxed);
    if (end > previous) atomic_store_explicit(&shared->high_water, end, memory_order_release);
    mix_unlock();
    return true;
}

UInt32 ns6_transport_available_shared(const NS6TransportShared *shared) {
    if (!shared || !atomic_load_explicit(&shared->initialized, memory_order_acquire)) return 0;
    uint64_t cursor = atomic_load_explicit(&shared->read_cursor, memory_order_acquire);
    uint64_t end = atomic_load_explicit(&shared->high_water, memory_order_acquire);
    if (end <= cursor) return 0;
    uint64_t limit = end - cursor;
    if (limit > NS6_TRANSPORT_RING_FRAMES) limit = NS6_TRANSPORT_RING_FRAMES;
    UInt32 count = 0;
    while (count < limit) {
        UInt32 slot = (UInt32)((cursor + count) % NS6_TRANSPORT_RING_FRAMES);
        if (atomic_load_explicit(&shared->tags[slot], memory_order_acquire) != cursor + count) break;
        ++count;
    }
    return count;
}

static void interpolate_frame(const unsigned char *first, const unsigned char *second,
                              double fraction, unsigned char *output) {
    for (unsigned channel = 0; channel < CHANNELS; ++channel) {
        int32_t a = unpack24(first + channel * 3);
        int32_t b = unpack24(second + channel * 3);
        double value = (double)a + ((double)b - (double)a) * fraction;
        if (value > 8388607.0) value = 8388607.0;
        if (value < -8388608.0) value = -8388608.0;
        pack24(output + channel * 3, (int32_t)lrint(value));
    }
}

UInt32 ns6_transport_dequeue_resampled_shared_diagnostic(
    NS6TransportShared *shared, NS6TransportReader *reader, void *output,
    UInt32 output_frames, double source_frames_per_output_frame,
    NS6TransportStopReason *stop_reason) {
    if (stop_reason) *stop_reason = NS6_TRANSPORT_STOP_INVALID;
    if (!shared || !reader || !output || output_frames == 0 ||
        !atomic_load_explicit(&shared->initialized, memory_order_acquire)) return 0;
    if (stop_reason) *stop_reason = NS6_TRANSPORT_STOP_NONE;
    if (!isfinite(source_frames_per_output_frame) || source_frames_per_output_frame < 0.5 ||
        source_frames_per_output_frame > 1.5) source_frames_per_output_frame = 1.0;
    UInt64 cursor = atomic_load_explicit(&shared->read_cursor, memory_order_relaxed);
    UInt64 end = atomic_load_explicit(&shared->high_water, memory_order_acquire);
    unsigned char *destination = output;
    double phase = reader->read_fraction;
    UInt32 produced = 0;
    for (; produced < output_frames; ++produced) {
        UInt64 whole = (UInt64)phase;
        double fraction = phase - (double)whole;
        UInt64 first_position = cursor + whole;
        UInt64 second_position = first_position + (fraction > 0.0 ? 1u : 0u);
        if (second_position >= end) {
            if (stop_reason) *stop_reason = NS6_TRANSPORT_STOP_EMPTY;
            break;
        }
        if (second_position - cursor >= NS6_TRANSPORT_RING_FRAMES) {
            if (stop_reason) *stop_reason = NS6_TRANSPORT_STOP_RING_LIMIT;
            break;
        }
        UInt32 first_slot = (UInt32)(first_position % NS6_TRANSPORT_RING_FRAMES);
        UInt32 second_slot = (UInt32)(second_position % NS6_TRANSPORT_RING_FRAMES);
        if (atomic_load_explicit(&shared->tags[first_slot], memory_order_acquire) != first_position ||
            atomic_load_explicit(&shared->tags[second_slot], memory_order_acquire) != second_position) {
            if (stop_reason) *stop_reason = NS6_TRANSPORT_STOP_MISSING_TAG;
            break;
        }
        if (atomic_load_explicit(&shared->tags[first_slot], memory_order_relaxed) != first_position ||
            atomic_load_explicit(&shared->tags[second_slot], memory_order_relaxed) != second_position) {
            if (stop_reason) *stop_reason = NS6_TRANSPORT_STOP_MISSING_TAG;
            break;
        }
        const unsigned char *first = shared->samples + (size_t)first_slot * USB_FRAME_BYTES;
        const unsigned char *second = shared->samples + (size_t)second_slot * USB_FRAME_BYTES;
        interpolate_frame(first, second, fraction,
                          destination + (size_t)produced * USB_FRAME_BYTES);
        phase += source_frames_per_output_frame;
    }
    UInt64 consumed = (UInt64)phase;
    reader->read_fraction = phase - (double)consumed;
    atomic_store_explicit(&shared->read_cursor, cursor + consumed, memory_order_release);
    return produced;
}

UInt32 ns6_transport_dequeue_resampled_shared(NS6TransportShared *shared,
                                              NS6TransportReader *reader, void *output,
                                              UInt32 output_frames,
                                              double source_frames_per_output_frame) {
    return ns6_transport_dequeue_resampled_shared_diagnostic(
        shared, reader, output, output_frames, source_frames_per_output_frame, NULL);
}

UInt32 ns6_transport_dequeue_shared(NS6TransportShared *shared, NS6TransportReader *reader,
                                    void *frames, UInt32 capacity) {
    return ns6_transport_dequeue_resampled_shared(shared, reader, frames, capacity, 1.0);
}

UInt32 ns6_transport_advance_resampled_shared(NS6TransportShared *shared,
                                              NS6TransportReader *reader,
                                              UInt32 output_frames,
                                              double source_frames_per_output_frame) {
    if (!shared || !reader || !atomic_load_explicit(&shared->initialized, memory_order_acquire)) return 0;
    if (!isfinite(source_frames_per_output_frame) || source_frames_per_output_frame < 0.5 ||
        source_frames_per_output_frame > 1.5) source_frames_per_output_frame = 1.0;
    double phase = reader->read_fraction + (double)output_frames * source_frames_per_output_frame;
    UInt64 consumed = (UInt64)phase;
    reader->read_fraction = phase - (double)consumed;
    UInt64 cursor = atomic_load_explicit(&shared->read_cursor, memory_order_relaxed);
    atomic_store_explicit(&shared->read_cursor, cursor + consumed, memory_order_release);
    return (UInt32)consumed;
}

UInt32 ns6_transport_advance_shared(NS6TransportShared *shared, UInt32 frames) {
    if (!shared || !atomic_load_explicit(&shared->initialized, memory_order_acquire)) return 0;
    uint64_t cursor = atomic_load_explicit(&shared->read_cursor, memory_order_relaxed);
    atomic_store_explicit(&shared->read_cursor, cursor + frames, memory_order_release);
    return frames;
}

/* Existing in-process entry points remain source-compatible during migration. */
void ns6_transport_reset(void) {
    ns6_transport_reset_shared(active_shared);
    active_reader->read_fraction = 0.0;
}

void ns6_transport_discard(void) {
    ns6_transport_discard_shared(active_shared, active_reader);
}

bool ns6_transport_enqueue_at(const void *input, UInt64 sample_time, UInt32 count,
                              const AudioStreamBasicDescription *format) {
    return ns6_transport_enqueue_at_shared(active_shared, input, sample_time, count, format);
}

bool ns6_transport_enqueue(const void *input, UInt32 count,
                           const AudioStreamBasicDescription *format) {
    return ns6_transport_enqueue_shared(active_shared, input, count, format);
}

UInt32 ns6_transport_available(void) {
    return ns6_transport_available_shared(active_shared);
}

UInt32 ns6_transport_dequeue(void *output, UInt32 capacity) {
    return ns6_transport_dequeue_shared(active_shared, active_reader, output, capacity);
}

UInt32 ns6_transport_dequeue_resampled(void *output, UInt32 output_frames,
                                       double source_frames_per_output_frame) {
    return ns6_transport_dequeue_resampled_shared(active_shared, active_reader, output,
                                                  output_frames, source_frames_per_output_frame);
}

UInt32 ns6_transport_dequeue_resampled_diagnostic(
    void *output, UInt32 output_frames, double source_frames_per_output_frame,
    NS6TransportStopReason *stop_reason) {
    return ns6_transport_dequeue_resampled_shared_diagnostic(
        active_shared, active_reader, output, output_frames,
        source_frames_per_output_frame, stop_reason);
}

UInt32 ns6_transport_advance_resampled(UInt32 output_frames,
                                       double source_frames_per_output_frame) {
    return ns6_transport_advance_resampled_shared(active_shared, active_reader,
                                                  output_frames, source_frames_per_output_frame);
}

UInt32 ns6_transport_advance(UInt32 frames) {
    return ns6_transport_advance_shared(active_shared, frames);
}
