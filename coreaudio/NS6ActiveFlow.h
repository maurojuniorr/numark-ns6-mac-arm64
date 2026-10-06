#ifndef NS6_ACTIVE_FLOW_H
#define NS6_ACTIVE_FLOW_H

#include <stdatomic.h>
#include <stdint.h>

typedef struct {
    atomic_uint frames;
    atomic_uint client_id;
    atomic_uint_fast64_t last_callback_tick;
} NS6ActiveFlow;

typedef struct {
    unsigned frames;
    unsigned client_id;
} NS6ActiveFlowSnapshot;

static inline void ns6_active_flow_record(NS6ActiveFlow *flow, unsigned frames, unsigned client_id, uint64_t tick) {
    atomic_store_explicit(&flow->frames, frames, memory_order_relaxed);
    atomic_store_explicit(&flow->client_id, client_id, memory_order_relaxed);
    atomic_store_explicit(&flow->last_callback_tick, tick, memory_order_release);
}

static inline void ns6_active_flow_clear(NS6ActiveFlow *flow) {
    atomic_store_explicit(&flow->last_callback_tick, 0, memory_order_release);
    atomic_store_explicit(&flow->frames, 0, memory_order_relaxed);
    atomic_store_explicit(&flow->client_id, 0, memory_order_relaxed);
}

static inline NS6ActiveFlowSnapshot ns6_active_flow_snapshot(const NS6ActiveFlow *flow, uint64_t now, uint64_t max_age_ticks) {
    NS6ActiveFlowSnapshot result = {0};
    uint64_t last = atomic_load_explicit(&flow->last_callback_tick, memory_order_acquire);
    if (last && now >= last && now - last < max_age_ticks) {
        result.frames = atomic_load_explicit(&flow->frames, memory_order_relaxed);
        result.client_id = atomic_load_explicit(&flow->client_id, memory_order_relaxed);
    }
    return result;
}

#endif
