#ifndef NS6_HAL_MAPPING_H
#define NS6_HAL_MAPPING_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <sched.h>

typedef struct {
    _Atomic(void *) memory;
    atomic_uint_fast64_t session_id;
    atomic_uint readers;
} NS6HALMapping;

typedef struct {
    void *memory;
    uint64_t session_id;
} NS6HALMappingLease;

static inline void ns6_hal_mapping_init(NS6HALMapping *mapping) {
    atomic_init(&mapping->memory, NULL);
    atomic_init(&mapping->session_id, 0);
    atomic_init(&mapping->readers, 0);
}

static inline bool ns6_hal_mapping_acquire(NS6HALMapping *mapping,
                                           NS6HALMappingLease *lease) {
    if (!mapping || !lease) return false;
    atomic_fetch_add_explicit(&mapping->readers, 1, memory_order_acquire);
    void *memory = atomic_load_explicit(&mapping->memory, memory_order_acquire);
    if (!memory) {
        atomic_fetch_sub_explicit(&mapping->readers, 1, memory_order_release);
        return false;
    }
    lease->memory = memory;
    lease->session_id = atomic_load_explicit(&mapping->session_id, memory_order_acquire);
    return true;
}

static inline void ns6_hal_mapping_release(NS6HALMapping *mapping) {
    atomic_fetch_sub_explicit(&mapping->readers, 1, memory_order_release);
}

/* Called by the non-realtime owner before replacing or unmapping a client map. */
static inline void ns6_hal_mapping_withdraw(NS6HALMapping *mapping) {
    atomic_store_explicit(&mapping->memory, NULL, memory_order_release);
    while (atomic_load_explicit(&mapping->readers, memory_order_acquire) != 0)
        sched_yield();
}

/* Publish only after withdraw has completed and the new map is fully initialized. */
static inline void ns6_hal_mapping_publish(NS6HALMapping *mapping, void *memory,
                                           uint64_t session_id) {
    atomic_store_explicit(&mapping->session_id, session_id, memory_order_relaxed);
    atomic_store_explicit(&mapping->memory, memory, memory_order_release);
}

#endif
