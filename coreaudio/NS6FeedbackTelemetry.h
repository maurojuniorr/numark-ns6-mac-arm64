#ifndef NS6_FEEDBACK_TELEMETRY_H
#define NS6_FEEDBACK_TELEMETRY_H

#include <stdbool.h>
#include <stdint.h>

static inline bool ns6_feedback_elapsed_ms(uint64_t now_ms, uint64_t since_ms,
                                           uint64_t *elapsed_ms) {
    if (!since_ms || !elapsed_ms || now_ms < since_ms) return false;
    *elapsed_ms = now_ms - since_ms;
    return true;
}

#endif
