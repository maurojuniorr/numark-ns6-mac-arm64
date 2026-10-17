#include "NS6IOMetrics.h"

#include <string.h>

NS6IOMetrics ns6_io_metrics;

static void update_max(atomic_uint_fast64_t *target, uint64_t value) {
    uint64_t previous = atomic_load_explicit(target, memory_order_relaxed);
    while (value > previous && !atomic_compare_exchange_weak_explicit(
               target, &previous, value, memory_order_relaxed,
               memory_order_relaxed)) {}
}

void ns6_io_metrics_init(NS6IOMetrics *metrics) {
    if (!metrics) return;
    memset(metrics, 0, sizeof(*metrics));
    for (size_t i = 0; i < NS6_IO_METRICS_MAX_CLIENTS; ++i) {
        atomic_init(&metrics->clients[i].client_key, 0);
        atomic_init(&metrics->clients[i].calls, 0);
        atomic_init(&metrics->clients[i].frames, 0);
        atomic_init(&metrics->clients[i].last_cycle, 0);
        atomic_init(&metrics->clients[i].last_output_sample_time, 0);
        atomic_init(&metrics->clients[i].last_output_frames, 0);
        atomic_init(&metrics->clients[i].output_sample_time_valid, false);
        atomic_init(&metrics->clients[i].duplicate_cycles, 0);
        atomic_init(&metrics->clients[i].skipped_cycles, 0);
        atomic_init(&metrics->clients[i].last_callback_start_ns, 0);
        atomic_init(&metrics->clients[i].interval_count, 0);
        atomic_init(&metrics->clients[i].interval_total_ns, 0);
        atomic_init(&metrics->clients[i].max_interval_ns, 0);
        atomic_init(&metrics->clients[i].max_callback_ns, 0);
        atomic_init(&metrics->clients[i].deadline_misses, 0);
    }
}

static NS6IOClientMetrics *client_slot(NS6IOMetrics *metrics,
                                       uint32_t client_id) {
    const uint64_t key = (uint64_t)client_id + 1;
    for (size_t i = 0; i < NS6_IO_METRICS_MAX_CLIENTS; ++i) {
        NS6IOClientMetrics *slot = &metrics->clients[i];
        uint64_t current = atomic_load_explicit(&slot->client_key,
                                                 memory_order_relaxed);
        if (current == key) return slot;
        if (current == 0 && atomic_compare_exchange_strong_explicit(
                &slot->client_key, &current, key,
                memory_order_relaxed, memory_order_relaxed)) return slot;
    }
    return NULL;
}

void ns6_io_metrics_record(NS6IOMetrics *metrics, uint32_t client_id,
                           uint32_t frames, uint64_t cycle) {
    if (!metrics) return;
    NS6IOClientMetrics *slot = client_slot(metrics, client_id);
    if (!slot) return;

    atomic_fetch_add_explicit(&slot->calls, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&slot->frames, frames, memory_order_relaxed);
    uint64_t previous = atomic_exchange_explicit(&slot->last_cycle, cycle,
                                                  memory_order_relaxed);
    if (cycle != 0 && previous == cycle)
        atomic_fetch_add_explicit(&slot->duplicate_cycles, 1,
                                  memory_order_relaxed);
    if (cycle != 0 && previous != 0 && cycle > previous + 1 &&
        cycle - previous < 1024)
        atomic_fetch_add_explicit(&slot->skipped_cycles, cycle - previous - 1,
                                  memory_order_relaxed);
}

void ns6_io_metrics_record_timed(NS6IOMetrics *metrics, uint32_t client_id,
                                 uint32_t frames, uint64_t cycle,
                                 uint64_t start_ns, uint64_t callback_ns,
                                 uint64_t deadline_ns) {
    if (!metrics) return;
    NS6IOClientMetrics *slot = client_slot(metrics, client_id);
    if (!slot) return;

    atomic_fetch_add_explicit(&slot->calls, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&slot->frames, frames, memory_order_relaxed);
    uint64_t previous_cycle = atomic_exchange_explicit(
        &slot->last_cycle, cycle, memory_order_relaxed);
    if (cycle != 0 && previous_cycle == cycle)
        atomic_fetch_add_explicit(&slot->duplicate_cycles, 1,
                                  memory_order_relaxed);
    if (cycle != 0 && previous_cycle != 0 && cycle > previous_cycle + 1 &&
        cycle - previous_cycle < 1024)
        atomic_fetch_add_explicit(&slot->skipped_cycles,
                                  cycle - previous_cycle - 1,
                                  memory_order_relaxed);

    uint64_t previous = atomic_exchange_explicit(
        &slot->last_callback_start_ns, start_ns, memory_order_relaxed);
    if (start_ns && previous && start_ns > previous) {
        uint64_t interval_ns = start_ns - previous;
        atomic_fetch_add_explicit(&slot->interval_count, 1,
                                  memory_order_relaxed);
        atomic_fetch_add_explicit(&slot->interval_total_ns, interval_ns,
                                  memory_order_relaxed);
        update_max(&slot->max_interval_ns, interval_ns);
    }
    update_max(&slot->max_callback_ns, callback_ns);
    if (deadline_ns && callback_ns > deadline_ns)
        atomic_fetch_add_explicit(&slot->deadline_misses, 1,
                                  memory_order_relaxed);
}

void ns6_io_metrics_record_output_timed(NS6IOMetrics *metrics,
                                        uint32_t client_id, uint32_t frames,
                                        uint64_t cycle, uint64_t output_sample_time,
                                        bool output_sample_time_valid,
                                        uint64_t start_ns, uint64_t callback_ns,
                                        uint64_t deadline_ns) {
    ns6_io_metrics_record_timed(metrics, client_id, frames, cycle, start_ns,
                                callback_ns, deadline_ns);
    NS6IOClientMetrics *slot = client_slot(metrics, client_id);
    if (!slot) return;
    atomic_store_explicit(&slot->last_output_sample_time, output_sample_time,
                          memory_order_relaxed);
    atomic_store_explicit(&slot->last_output_frames, frames, memory_order_relaxed);
    atomic_store_explicit(&slot->output_sample_time_valid,
                          output_sample_time_valid, memory_order_relaxed);
}

size_t ns6_io_metrics_take_snapshot(NS6IOMetrics *metrics,
                                    NS6IOMetricsSnapshot *snapshots,
                                    size_t capacity) {
    if (!metrics || !snapshots || capacity == 0) return 0;
    size_t count = 0;
    for (size_t i = 0; i < NS6_IO_METRICS_MAX_CLIENTS && count < capacity; ++i) {
        NS6IOClientMetrics *slot = &metrics->clients[i];
        uint64_t key = atomic_load_explicit(&slot->client_key,
                                             memory_order_relaxed);
        if (key == 0) continue;
        NS6IOMetricsSnapshot *snapshot = &snapshots[count++];
        snapshot->client_id = (uint32_t)(key - 1);
        snapshot->calls = atomic_exchange_explicit(&slot->calls, 0,
                                                    memory_order_relaxed);
        snapshot->frames = atomic_exchange_explicit(&slot->frames, 0,
                                                     memory_order_relaxed);
        snapshot->last_cycle = atomic_load_explicit(&slot->last_cycle,
                                                     memory_order_relaxed);
        snapshot->last_output_sample_time = atomic_load_explicit(
            &slot->last_output_sample_time, memory_order_relaxed);
        snapshot->last_output_frames = atomic_load_explicit(
            &slot->last_output_frames, memory_order_relaxed);
        snapshot->output_sample_time_valid = atomic_load_explicit(
            &slot->output_sample_time_valid, memory_order_relaxed);
        snapshot->duplicate_cycles = atomic_exchange_explicit(
            &slot->duplicate_cycles, 0, memory_order_relaxed);
        snapshot->skipped_cycles = atomic_exchange_explicit(
            &slot->skipped_cycles, 0, memory_order_relaxed);
        snapshot->interval_count = atomic_exchange_explicit(
            &slot->interval_count, 0, memory_order_relaxed);
        snapshot->interval_total_ns = atomic_exchange_explicit(
            &slot->interval_total_ns, 0, memory_order_relaxed);
        snapshot->max_interval_ns = atomic_exchange_explicit(
            &slot->max_interval_ns, 0, memory_order_relaxed);
        snapshot->max_callback_ns = atomic_exchange_explicit(
            &slot->max_callback_ns, 0, memory_order_relaxed);
        snapshot->deadline_misses = atomic_exchange_explicit(
            &slot->deadline_misses, 0, memory_order_relaxed);
    }
    return count;
}
