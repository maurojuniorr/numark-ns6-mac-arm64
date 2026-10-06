#ifndef NS6_IO_METRICS_H
#define NS6_IO_METRICS_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NS6_IO_METRICS_MAX_CLIENTS 32u

typedef struct {
    atomic_uint_fast64_t client_key;
    atomic_uint_fast64_t calls;
    atomic_uint_fast64_t frames;
    atomic_uint_fast64_t last_cycle;
    atomic_uint_fast64_t duplicate_cycles;
    atomic_uint_fast64_t skipped_cycles;
} NS6IOClientMetrics;

typedef struct {
    NS6IOClientMetrics clients[NS6_IO_METRICS_MAX_CLIENTS];
} NS6IOMetrics;

typedef struct {
    uint32_t client_id;
    uint64_t calls;
    uint64_t frames;
    uint64_t last_cycle;
    uint64_t duplicate_cycles;
    uint64_t skipped_cycles;
} NS6IOMetricsSnapshot;

void ns6_io_metrics_init(NS6IOMetrics *metrics);
void ns6_io_metrics_record(NS6IOMetrics *metrics, uint32_t client_id,
                           uint32_t frames, uint64_t cycle);
size_t ns6_io_metrics_take_snapshot(NS6IOMetrics *metrics,
                                    NS6IOMetricsSnapshot *snapshots,
                                    size_t capacity);

#endif
