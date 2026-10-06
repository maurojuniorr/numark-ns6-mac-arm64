#include "NS6Transport.h"
#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

#define CHANNELS 4
#define USB_FRAME_BYTES 12

/* CoreAudio timestamps select ring positions; tags distinguish wrapped slots. */
static unsigned char ring[NS6_TRANSPORT_RING_FRAMES * USB_FRAME_BYTES];
static _Atomic uint64_t tags[NS6_TRANSPORT_RING_FRAMES];
static _Atomic uint64_t read_cursor;
static _Atomic uint64_t high_water;
static atomic_bool initialized;
/* Only the USB worker accesses this fractional source-frame phase. */
static double read_fraction;

static void pack24(unsigned char *out, int32_t sample) {
    out[0] = (unsigned char)sample;
    out[1] = (unsigned char)(sample >> 8);
    out[2] = (unsigned char)(sample >> 16);
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

void ns6_transport_reset(void) {
    for (UInt32 i = 0; i < NS6_TRANSPORT_RING_FRAMES; ++i)
        atomic_store_explicit(&tags[i], UINT64_MAX, memory_order_relaxed);
    atomic_store_explicit(&read_cursor, 0, memory_order_relaxed);
    atomic_store_explicit(&high_water, 0, memory_order_relaxed);
    read_fraction = 0.0;
    atomic_store_explicit(&initialized, false, memory_order_release);
}

void ns6_transport_discard(void) {
    if (!atomic_exchange_explicit(&initialized, false, memory_order_acq_rel)) return;
    for (UInt32 i = 0; i < NS6_TRANSPORT_RING_FRAMES; ++i)
        atomic_store_explicit(&tags[i], UINT64_MAX, memory_order_relaxed);
    atomic_store_explicit(&read_cursor, 0, memory_order_relaxed);
    atomic_store_explicit(&high_water, 0, memory_order_relaxed);
    read_fraction = 0.0;
}

bool ns6_transport_enqueue_at(const void *input, UInt64 sample_time, UInt32 count,
                              const AudioStreamBasicDescription *format) {
    if ((!input && count) || !valid_format(format)) return false;
    if (count == 0) return true;
    /* CoreAudio's output timeline is not the NS6 USB clock's FIFO position.
       Append each completed mix block in order; the USB clock consumes it. */
    sample_time = atomic_load_explicit(&high_water, memory_order_acquire);
    if (sample_time > UINT64_MAX - count) return false;

    if (!atomic_load_explicit(&initialized, memory_order_acquire)) {
        atomic_store_explicit(&read_cursor, sample_time, memory_order_relaxed);
        atomic_store_explicit(&high_water, sample_time, memory_order_relaxed);
        atomic_store_explicit(&initialized, true, memory_order_release);
    }

    uint64_t cursor = atomic_load_explicit(&read_cursor, memory_order_acquire);
    /* If the USB reader advanced through a starvation gap, resume appending at
       the current read point instead of silently discarding new CoreAudio data
       until the old high-water mark catches up. */
    if (sample_time < cursor) {
        sample_time = cursor;
        atomic_store_explicit(&high_water, cursor, memory_order_release);
    }
    uint64_t end = sample_time + count;
    if (end <= cursor) return true; /* This cycle arrived after its samples were played. */
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
        unsigned char *destination = ring + (size_t)slot * USB_FRAME_BYTES;
        convert_frame(source + (size_t)frame * format->mBytesPerFrame, sample_bytes,
                      is_float, format->mBitsPerChannel, destination);
        atomic_store_explicit(&tags[slot], position, memory_order_release);
    }
    uint64_t previous = atomic_load_explicit(&high_water, memory_order_relaxed);
    while (end > previous && !atomic_compare_exchange_weak_explicit(
               &high_water, &previous, end, memory_order_release, memory_order_relaxed)) {}
    return true;
}

bool ns6_transport_enqueue(const void *input, UInt32 count,
                           const AudioStreamBasicDescription *format) {
    UInt64 position = atomic_load_explicit(&high_water, memory_order_acquire);
    if (!atomic_load_explicit(&initialized, memory_order_acquire)) position = 0;
    return ns6_transport_enqueue_at(input, position, count, format);
}

UInt32 ns6_transport_available(void) {
    if (!atomic_load_explicit(&initialized, memory_order_acquire)) return 0;
    uint64_t cursor = atomic_load_explicit(&read_cursor, memory_order_acquire);
    uint64_t end = atomic_load_explicit(&high_water, memory_order_acquire);
    if (end <= cursor) return 0;
    uint64_t limit = end - cursor;
    if (limit > NS6_TRANSPORT_RING_FRAMES) limit = NS6_TRANSPORT_RING_FRAMES;
    UInt32 count = 0;
    while (count < limit) {
        UInt32 slot = (UInt32)((cursor + count) % NS6_TRANSPORT_RING_FRAMES);
        if (atomic_load_explicit(&tags[slot], memory_order_acquire) != cursor + count) break;
        ++count;
    }
    return count;
}

UInt32 ns6_transport_dequeue(void *output, UInt32 capacity) {
    return ns6_transport_dequeue_resampled(output, capacity, 1.0);
}

static int32_t unpack24(const unsigned char *sample) {
    return (int32_t)sample[0] | ((int32_t)sample[1] << 8) |
           ((int32_t)(int8_t)sample[2] << 16);
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

UInt32 ns6_transport_dequeue_resampled(void *output, UInt32 output_frames,
                                       double source_frames_per_output_frame) {
    if (!output || output_frames == 0 || !atomic_load_explicit(&initialized, memory_order_acquire)) return 0;
    if (!isfinite(source_frames_per_output_frame) || source_frames_per_output_frame < 0.5 ||
        source_frames_per_output_frame > 1.5) source_frames_per_output_frame = 1.0;
    UInt64 cursor = atomic_load_explicit(&read_cursor, memory_order_relaxed);
    UInt64 end = atomic_load_explicit(&high_water, memory_order_acquire);
    unsigned char *destination = output;
    double phase = read_fraction;
    UInt32 produced = 0;
    for (; produced < output_frames; ++produced) {
        UInt64 whole = (UInt64)phase;
        double fraction = phase - (double)whole;
        UInt64 first_position = cursor + whole;
        UInt64 second_position = first_position + (fraction > 0.0 ? 1u : 0u);
        if (second_position >= end || second_position - cursor >= NS6_TRANSPORT_RING_FRAMES) break;
        UInt32 first_slot = (UInt32)(first_position % NS6_TRANSPORT_RING_FRAMES);
        UInt32 second_slot = (UInt32)(second_position % NS6_TRANSPORT_RING_FRAMES);
        if (atomic_load_explicit(&tags[first_slot], memory_order_acquire) != first_position ||
            atomic_load_explicit(&tags[second_slot], memory_order_acquire) != second_position) break;
        const unsigned char *first = ring + (size_t)first_slot * USB_FRAME_BYTES;
        const unsigned char *second = ring + (size_t)second_slot * USB_FRAME_BYTES;
        interpolate_frame(first, second, fraction,
                          destination + (size_t)produced * USB_FRAME_BYTES);
        phase += source_frames_per_output_frame;
    }
    UInt64 consumed = (UInt64)phase;
    read_fraction = phase - (double)consumed;
    atomic_store_explicit(&read_cursor, cursor + consumed, memory_order_release);
    return produced;
}

UInt32 ns6_transport_advance_resampled(UInt32 output_frames,
                                       double source_frames_per_output_frame) {
    if (!atomic_load_explicit(&initialized, memory_order_acquire)) return 0;
    if (!isfinite(source_frames_per_output_frame) || source_frames_per_output_frame < 0.5 ||
        source_frames_per_output_frame > 1.5) source_frames_per_output_frame = 1.0;
    double phase = read_fraction + (double)output_frames * source_frames_per_output_frame;
    UInt64 consumed = (UInt64)phase;
    read_fraction = phase - (double)consumed;
    UInt64 cursor = atomic_load_explicit(&read_cursor, memory_order_relaxed);
    atomic_store_explicit(&read_cursor, cursor + consumed, memory_order_release);
    return (UInt32)consumed;
}

UInt32 ns6_transport_advance(UInt32 frames) {
    if (!atomic_load_explicit(&initialized, memory_order_acquire)) return 0;
    uint64_t cursor = atomic_load_explicit(&read_cursor, memory_order_relaxed);
    atomic_store_explicit(&read_cursor, cursor + frames, memory_order_release);
    return frames;
}
