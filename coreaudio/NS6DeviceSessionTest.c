#include "NS6DeviceSession.h"

#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>

typedef struct {
    NS6DeviceSession *session;
    atomic_bool finished;
} Handoff;

static void *replace_session(void *context) {
    Handoff *handoff = context;
    assert(ns6_device_session_begin(handoff->session, 202));
    atomic_store(&handoff->finished, true);
    return NULL;
}

int main(void) {
    NS6DeviceSession session;
    ns6_device_session_init(&session);
    assert(ns6_device_session_begin(&session, 101));
    assert(ns6_device_session_is_current(&session, 101));
    assert(ns6_device_session_io_begin(&session, 101));

    Handoff handoff = {.session = &session};
    atomic_init(&handoff.finished, false);
    pthread_t thread;
    assert(pthread_create(&thread, NULL, replace_session, &handoff) == 0);
    while (atomic_load(&session.active_id) != 0) sched_yield();
    assert(!ns6_device_session_io_begin(&session, 101));
    assert(!atomic_load(&handoff.finished));
    ns6_device_session_io_end(&session);
    assert(pthread_join(thread, NULL) == 0);

    assert(ns6_device_session_is_current(&session, 202));
    assert(!ns6_device_session_is_current(&session, 101));
    assert(!ns6_device_session_io_begin(&session, 101));
    assert(ns6_device_session_io_begin(&session, 202));
    ns6_device_session_io_end(&session);
    puts("NS6 HAL session handoff tests passed");
    return 0;
}
