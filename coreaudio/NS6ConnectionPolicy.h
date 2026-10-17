#ifndef NS6_CONNECTION_POLICY_H
#define NS6_CONNECTION_POLICY_H

#include <stdbool.h>

static inline bool ns6_connection_should_defer_disconnect(bool physically_connected,
                                                           unsigned active_clients) {
    return !physically_connected && active_clients > 0;
}

#endif
