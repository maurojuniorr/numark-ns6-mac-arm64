#include "NS6SharedMemory.h"

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include <sched.h>

static AudioStreamBasicDescription float_format(void) {
    AudioStreamBasicDescription format = {0};
    format.mSampleRate = 44100.0;
    format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagsNativeEndian | kAudioFormatFlagIsPacked;
    format.mBytesPerPacket = 16;
    format.mFramesPerPacket = 1;
    format.mBytesPerFrame = 16;
    format.mChannelsPerFrame = 4;
    format.mBitsPerChannel = 32;
    return format;
}

static void fill_frames(float *frames, uint32_t count, uint32_t first) {
    for (uint32_t frame = 0; frame < count; ++frame) {
        float value = (float)((first + frame) % 1000) / 1000.0f;
        for (unsigned channel = 0; channel < 4; ++channel)
            frames[(size_t)frame * 4 + channel] = value;
    }
}

static float decode_sample(const unsigned char *sample) {
    int32_t value = (int32_t)sample[0] | ((int32_t)sample[1] << 8) |
                    ((int32_t)(int8_t)sample[2] << 16);
    return (float)value / 8388607.0f;
}

static void test_shared_output_ring_preserves_order_across_wrap(void) {
    NS6TransportShared *ring = calloc(1, sizeof(*ring));
    assert(ring != NULL);
    ns6_transport_init_shared(ring);
    AudioStreamBasicDescription format = float_format();
    float *input = malloc((size_t)NS6_TRANSPORT_RING_FRAMES * 4 * sizeof(float));
    unsigned char *output = malloc((size_t)NS6_TRANSPORT_RING_FRAMES * 12);
    assert(input && output);

    fill_frames(input, NS6_TRANSPORT_RING_FRAMES, 0);
    assert(ns6_transport_enqueue_shared(ring, input, NS6_TRANSPORT_RING_FRAMES, &format));
    NS6TransportReader reader = {0};
    assert(ns6_transport_dequeue_shared(ring, &reader, output, NS6_TRANSPORT_RING_FRAMES) == NS6_TRANSPORT_RING_FRAMES);
    for (uint32_t frame = 0; frame < NS6_TRANSPORT_RING_FRAMES; ++frame)
        assert(fabsf(decode_sample(output + (size_t)frame * 12) - (float)(frame % 1000) / 1000.0f) < 0.000002f);

    fill_frames(input, 8, NS6_TRANSPORT_RING_FRAMES);
    assert(ns6_transport_enqueue_shared(ring, input, 8, &format));
    assert(ns6_transport_dequeue_shared(ring, &reader, output, 8) == 8);
    for (uint32_t frame = 0; frame < 8; ++frame)
        assert(fabsf(decode_sample(output + (size_t)frame * 12) - (float)((NS6_TRANSPORT_RING_FRAMES + frame) % 1000) / 1000.0f) < 0.000002f);

    free(output);
    free(input);
    free(ring);
}

static void test_shared_capture_ring_reports_empty_and_full(void) {
    NS6CaptureRing *ring = calloc(1, sizeof(*ring));
    assert(ring != NULL);
    ns6_capture_ring_init(ring);
    float *input = malloc((size_t)(NS6_CAPTURE_RING_CAPACITY + 1) * 2 * sizeof(float));
    float *output = malloc((size_t)NS6_CAPTURE_RING_CAPACITY * 2 * sizeof(float));
    assert(input && output);
    for (size_t i = 0; i < (size_t)(NS6_CAPTURE_RING_CAPACITY + 1) * 2; ++i) input[i] = (float)i;

    assert(ns6_capture_ring_read(ring, output, 1) == 0);
    assert(output[0] == 0.0f && output[1] == 0.0f);
    assert(ns6_capture_ring_write(ring, input, NS6_CAPTURE_RING_CAPACITY) == NS6_CAPTURE_RING_CAPACITY);
    assert(ns6_capture_ring_write(ring, input + (size_t)NS6_CAPTURE_RING_CAPACITY * 2, 1) == 0);
    assert(ns6_capture_ring_read(ring, output, NS6_CAPTURE_RING_CAPACITY) == NS6_CAPTURE_RING_CAPACITY);
    assert(output[0] == input[0] && output[NS6_CAPTURE_RING_CAPACITY * 2 - 1] == input[NS6_CAPTURE_RING_CAPACITY * 2 - 1]);

    free(output);
    free(input);
    free(ring);
}

static void test_capture_ring_cross_process_and_silence_fill(void) {
    enum { FRAMES = 100000, BLOCK = 64 };
    NS6SharedMemory *memory = mmap(NULL, sizeof(*memory), PROT_READ | PROT_WRITE,
                                   MAP_SHARED | MAP_ANON, -1, 0);
    assert(memory != MAP_FAILED);
    memset(memory, 0, sizeof(*memory));
    ns6_shared_memory_init(memory);

    pid_t producer = fork();
    assert(producer >= 0);
    if (producer == 0) {
        float input[BLOCK * 2];
        for (uint32_t first = 0; first < FRAMES; first += BLOCK) {
            uint32_t count = FRAMES - first < BLOCK ? FRAMES - first : BLOCK;
            for (uint32_t frame = 0; frame < count; ++frame) {
                input[frame * 2] = (float)(first + frame);
                input[frame * 2 + 1] = -(float)(first + frame);
            }
            size_t offset = 0;
            while (offset < count) {
                size_t accepted = ns6_capture_ring_write(&memory->capture,
                                                         input + offset * 2, count - offset);
                offset += accepted;
                if (!accepted) sched_yield();
            }
        }
        _exit(0);
    }

    float output[BLOCK * 2];
    uint32_t consumed = 0;
    while (consumed < FRAMES) {
        uint32_t request = FRAMES - consumed < BLOCK ? FRAMES - consumed : BLOCK;
        size_t count = ns6_capture_ring_read(&memory->capture, output, request);
        if (!count) {
            sched_yield();
            continue;
        }
        for (size_t frame = 0; frame < count; ++frame) {
            float expected = (float)(consumed + frame);
            assert(output[frame * 2] == expected);
            assert(output[frame * 2 + 1] == -expected);
        }
        consumed += (uint32_t)count;
    }
    assert(ns6_capture_ring_read(&memory->capture, output, 1) == 0);
    assert(output[0] == 0.0f && output[1] == 0.0f);

    int status = 0;
    assert(waitpid(producer, &status, 0) == producer);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    assert(munmap(memory, sizeof(*memory)) == 0);
}

static void test_shared_ring_two_processes(void) {
    enum { FRAMES = 1000000, BLOCK = 64 };
    NS6SharedMemory *memory = mmap(NULL, sizeof(*memory), PROT_READ | PROT_WRITE,
                                   MAP_SHARED | MAP_ANON, -1, 0);
    assert(memory != MAP_FAILED);
    memset(memory, 0, sizeof(*memory));
    ns6_shared_memory_init(memory);
    assert(ns6_shared_memory_is_lock_free(memory));

    pid_t producer = fork();
    assert(producer >= 0);
    if (producer == 0) {
        AudioStreamBasicDescription format = float_format();
        float input[BLOCK * 4];
        for (uint32_t first = 0; first < FRAMES; first += BLOCK) {
            uint32_t count = FRAMES - first < BLOCK ? FRAMES - first : BLOCK;
            fill_frames(input, count, first);
            while (!ns6_transport_enqueue_shared(&memory->output, input, count, &format))
                sched_yield();
        }
        _exit(0);
    }

    NS6TransportReader reader = {0};
    unsigned char output[BLOCK * 12];
    uint32_t consumed = 0;
    while (consumed < FRAMES) {
        uint32_t request = FRAMES - consumed < BLOCK ? FRAMES - consumed : BLOCK;
        uint32_t count = ns6_transport_dequeue_shared(&memory->output, &reader, output, request);
        if (!count) {
            sched_yield();
            continue;
        }
        for (uint32_t frame = 0; frame < count; ++frame) {
            float expected = (float)((consumed + frame) % 1000) / 1000.0f;
            assert(fabsf(decode_sample(output + (size_t)frame * 12) - expected) < 0.000002f);
        }
        consumed += count;
    }

    int status = 0;
    assert(waitpid(producer, &status, 0) == producer);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    assert(munmap(memory, sizeof(*memory)) == 0);
}

int main(void) {
    test_shared_output_ring_preserves_order_across_wrap();
    test_shared_capture_ring_reports_empty_and_full();
    test_shared_ring_two_processes();
    test_capture_ring_cross_process_and_silence_fill();
    puts("NS6 shared-memory ring tests passed");
    return 0;
}
