#ifndef NS6_FEEDBACK_READ_GATE_H
#define NS6_FEEDBACK_READ_GATE_H

#include <stdbool.h>

/* Keep clock feedback flowing while USB transport runs, even when audio is paused. */
static inline bool ns6_feedback_read_should_submit(bool running, bool in_flight) {
    return running && !in_flight;
}

#endif
