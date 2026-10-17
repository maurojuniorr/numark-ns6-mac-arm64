#ifndef NS6_DEVICE_CLIENT_REGISTRY_H
#define NS6_DEVICE_CLIENT_REGISTRY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define NS6_DEVICE_MAX_CLIENTS 8

typedef struct {
    uint64_t client_id;
    int32_t process_id;
    bool audio_active;
} NS6DeviceClientLease;

typedef struct {
    NS6DeviceClientLease clients[NS6_DEVICE_MAX_CLIENTS];
} NS6DeviceClientRegistry;

static inline bool ns6_device_client_registry_attach(NS6DeviceClientRegistry *registry,
                                                      uint64_t client_id,
                                                      int32_t process_id) {
    if (!registry || client_id == 0) return false;
    for (size_t i = 0; i < NS6_DEVICE_MAX_CLIENTS; ++i) {
        if (registry->clients[i].client_id == client_id) {
            registry->clients[i].process_id = process_id;
            return true;
        }
    }
    for (size_t i = 0; i < NS6_DEVICE_MAX_CLIENTS; ++i) {
        if (registry->clients[i].client_id == 0) {
            registry->clients[i].client_id = client_id;
            registry->clients[i].process_id = process_id;
            registry->clients[i].audio_active = false;
            return true;
        }
    }
    return false;
}

static inline bool ns6_device_client_registry_set_audio_active(NS6DeviceClientRegistry *registry,
                                                                uint64_t client_id,
                                                                bool audio_active) {
    if (!registry || client_id == 0) return false;
    for (size_t i = 0; i < NS6_DEVICE_MAX_CLIENTS; ++i) {
        if (registry->clients[i].client_id == client_id) {
            registry->clients[i].audio_active = audio_active;
            return true;
        }
    }
    return false;
}

static inline bool ns6_device_client_registry_contains(const NS6DeviceClientRegistry *registry,
                                                        uint64_t client_id) {
    if (!registry || client_id == 0) return false;
    for (size_t i = 0; i < NS6_DEVICE_MAX_CLIENTS; ++i) {
        if (registry->clients[i].client_id == client_id) return true;
    }
    return false;
}

static inline bool ns6_device_client_registry_detach(NS6DeviceClientRegistry *registry,
                                                      uint64_t client_id) {
    if (!registry || client_id == 0) return false;
    for (size_t i = 0; i < NS6_DEVICE_MAX_CLIENTS; ++i) {
        if (registry->clients[i].client_id == client_id) {
            memset(&registry->clients[i], 0, sizeof(registry->clients[i]));
            return true;
        }
    }
    return false;
}

static inline size_t ns6_device_client_registry_detach_process(NS6DeviceClientRegistry *registry,
                                                                int32_t process_id) {
    if (!registry || process_id <= 0) return 0;
    size_t detached = 0;
    for (size_t i = 0; i < NS6_DEVICE_MAX_CLIENTS; ++i) {
        if (registry->clients[i].client_id != 0 &&
            registry->clients[i].process_id == process_id) {
            memset(&registry->clients[i], 0, sizeof(registry->clients[i]));
            ++detached;
        }
    }
    return detached;
}

static inline bool ns6_device_client_registry_any_audio_active(
    const NS6DeviceClientRegistry *registry) {
    if (!registry) return false;
    for (size_t i = 0; i < NS6_DEVICE_MAX_CLIENTS; ++i) {
        if (registry->clients[i].client_id != 0 && registry->clients[i].audio_active) return true;
    }
    return false;
}

#endif
