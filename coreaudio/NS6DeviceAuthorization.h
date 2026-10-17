#ifndef NS6_DEVICE_AUTHORIZATION_H
#define NS6_DEVICE_AUTHORIZATION_H

#include <stdbool.h>
#include <sys/types.h>

static inline bool ns6_device_peer_uid_authorized(uid_t peer_uid, uid_t coreaudiod_uid) {
    return peer_uid == 0 || (coreaudiod_uid != (uid_t)-1 && peer_uid == coreaudiod_uid);
}

#endif
