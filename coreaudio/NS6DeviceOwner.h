#ifndef NS6_DEVICE_OWNER_H
#define NS6_DEVICE_OWNER_H

#include <stdatomic.h>
#include <stdbool.h>

typedef struct {
    atomic_bool claimed;
} NS6DeviceOwner;

static inline void ns6_device_owner_init(NS6DeviceOwner *owner) {
    if (owner) atomic_init(&owner->claimed, false);
}

static inline bool ns6_device_owner_claim(NS6DeviceOwner *owner) {
    if (!owner) return false;
    bool expected = false;
    return atomic_compare_exchange_strong_explicit(&owner->claimed, &expected, true,
                                                   memory_order_acq_rel,
                                                   memory_order_acquire);
}

static inline void ns6_device_owner_release(NS6DeviceOwner *owner) {
    if (owner) atomic_store_explicit(&owner->claimed, false, memory_order_release);
}

static inline bool ns6_device_owner_is_claimed(const NS6DeviceOwner *owner) {
    return owner && atomic_load_explicit(&owner->claimed, memory_order_acquire);
}

#endif
