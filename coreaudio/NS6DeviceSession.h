#ifndef NS6_DEVICE_SESSION_H
#define NS6_DEVICE_SESSION_H

#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    atomic_uint_fast64_t active_id;
    atomic_uint callbacks_in_flight;
} NS6DeviceSession;

static inline void ns6_device_session_init(NS6DeviceSession *session) {
    if (!session) return;
    atomic_init(&session->active_id, 0);
    atomic_init(&session->callbacks_in_flight, 0);
}

static inline bool ns6_device_session_begin(NS6DeviceSession *session, uint64_t id) {
    if (!session || id == 0) return false;
    uint64_t previous = atomic_exchange_explicit(&session->active_id, 0, memory_order_acq_rel);
    for (unsigned attempt = 0; attempt < 100000; ++attempt) {
        if (atomic_load_explicit(&session->callbacks_in_flight, memory_order_acquire) == 0) {
            atomic_store_explicit(&session->active_id, id, memory_order_release);
            return true;
        }
        sched_yield();
    }
    atomic_store_explicit(&session->active_id, previous, memory_order_release);
    return false;
}

static inline bool ns6_device_session_is_current(const NS6DeviceSession *session, uint64_t id) {
    return session && id != 0 &&
           atomic_load_explicit(&session->active_id, memory_order_acquire) == id;
}

static inline bool ns6_device_session_io_begin(NS6DeviceSession *session, uint64_t id) {
    if (!session || id == 0) return false;
    atomic_fetch_add_explicit(&session->callbacks_in_flight, 1, memory_order_acquire);
    if (ns6_device_session_is_current(session, id)) return true;
    atomic_fetch_sub_explicit(&session->callbacks_in_flight, 1, memory_order_release);
    return false;
}

static inline void ns6_device_session_io_end(NS6DeviceSession *session) {
    if (session) atomic_fetch_sub_explicit(&session->callbacks_in_flight, 1, memory_order_release);
}

#endif
