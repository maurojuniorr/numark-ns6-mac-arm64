#include "NS6DeviceOwner.h"
#include "NS6SharedMemory.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static float decode_sample(const unsigned char *sample) {
    int32_t value = (int32_t)sample[0] | ((int32_t)sample[1] << 8) |
                    ((int32_t)(int8_t)sample[2] << 16);
    return (float)value / 8388607.0f;
}

static void test_owner_state_allows_one_usb_worker(void) {
    NS6DeviceOwner owner;
    ns6_device_owner_init(&owner);
    assert(ns6_device_owner_claim(&owner));
    assert(!ns6_device_owner_claim(&owner));
    assert(ns6_device_owner_is_claimed(&owner));
}

static void test_disconnect_releases_owner(void) {
    NS6DeviceOwner owner;
    ns6_device_owner_init(&owner);
    assert(ns6_device_owner_claim(&owner));
    ns6_device_owner_release(&owner);
    assert(!ns6_device_owner_is_claimed(&owner));
    assert(ns6_device_owner_claim(&owner));
}

static void test_usb_transport_uses_service_shared_ring(void) {
    NS6SharedMemory *memory = calloc(1, sizeof(*memory));
    assert(memory != NULL);
    ns6_shared_memory_init(memory);
    NS6TransportReader reader = {0};
    ns6_transport_bind_shared(&memory->output, &reader);
    ns6_transport_reset();

    AudioStreamBasicDescription format = {0};
    format.mSampleRate = 44100.0;
    format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagsNativeEndian | kAudioFormatFlagIsPacked;
    format.mBytesPerPacket = 16;
    format.mFramesPerPacket = 1;
    format.mBytesPerFrame = 16;
    format.mChannelsPerFrame = 4;
    format.mBitsPerChannel = 32;
    const float input[8] = {0.25f, 0.25f, 0.25f, 0.25f, 0.5f, 0.5f, 0.5f, 0.5f};
    unsigned char output[24] = {0};
    assert(ns6_transport_enqueue(input, 2, &format));
    assert(ns6_transport_available_shared(&memory->output) == 2);
    assert(ns6_transport_dequeue(output, 2) == 2);
    assert(fabsf(decode_sample(output) - 0.25f) < 0.000002f);
    assert(fabsf(decode_sample(output + 12) - 0.5f) < 0.000002f);
    assert(ns6_transport_available_shared(&memory->output) == 0);

    ns6_transport_bind_shared(NULL, NULL);
    free(memory);
}

int main(void) {
    test_owner_state_allows_one_usb_worker();
    test_disconnect_releases_owner();
    test_usb_transport_uses_service_shared_ring();
    puts("NS6 USB owner policy tests passed");
    return 0;
}
