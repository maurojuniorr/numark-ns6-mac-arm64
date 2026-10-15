#ifndef NS6_SHARED_MEMORY_H
#define NS6_SHARED_MEMORY_H

#include "NS6CaptureRing.h"
#include "NS6DeviceSession.h"
#include "NS6Transport.h"

#include <stdint.h>

#define NS6_SHARED_MEMORY_PROTOCOL_VERSION 3u

typedef struct {
    uint32_t protocol_version;
    uint32_t struct_size;
    NS6TransportShared output;
    NS6CaptureRing capture;
    NS6DeviceSession session;
    atomic_bool capture_requested;
    atomic_uint_fast64_t capture_last_demand_ns;
} NS6SharedMemory;

static inline void ns6_shared_memory_init(NS6SharedMemory *memory) {
    if (!memory) return;
    memory->protocol_version = NS6_SHARED_MEMORY_PROTOCOL_VERSION;
    memory->struct_size = (uint32_t)sizeof(*memory);
    ns6_transport_init_shared(&memory->output);
    ns6_capture_ring_init(&memory->capture);
    ns6_device_session_init(&memory->session);
    atomic_init(&memory->capture_requested, false);
    atomic_init(&memory->capture_last_demand_ns, 0);
}

static inline int ns6_shared_memory_is_lock_free(const NS6SharedMemory *memory) {
    return memory && atomic_is_lock_free(&memory->output.read_cursor) &&
           atomic_is_lock_free(&memory->output.high_water) &&
           atomic_is_lock_free(&memory->output.initialized) &&
           atomic_is_lock_free(&memory->capture.head) &&
           atomic_is_lock_free(&memory->capture.tail) &&
           atomic_is_lock_free(&memory->session.active_id) &&
           atomic_is_lock_free(&memory->session.callbacks_in_flight) &&
           atomic_is_lock_free(&memory->capture_requested) &&
           atomic_is_lock_free(&memory->capture_last_demand_ns);
}

#endif
