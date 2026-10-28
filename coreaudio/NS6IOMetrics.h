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
    atomic_uint_fast64_t last_output_sample_time;
    atomic_uint last_output_frames;
    atomic_bool output_sample_time_valid;
    atomic_uint_fast64_t duplicate_cycles;
    atomic_uint_fast64_t skipped_cycles;
    atomic_uint_fast64_t last_callback_start_ns;
    atomic_uint_fast64_t interval_count;
    atomic_uint_fast64_t interval_total_ns;
    atomic_uint_fast64_t max_interval_ns;
    atomic_uint_fast64_t max_callback_ns;
    atomic_uint_fast64_t deadline_misses;
} NS6IOClientMetrics;

typedef struct {
    NS6IOClientMetrics clients[NS6_IO_METRICS_MAX_CLIENTS];
} NS6IOMetrics;

typedef struct {
    uint32_t client_id;
    uint64_t calls;
    uint64_t frames;
    uint64_t last_cycle;
    uint64_t last_output_sample_time;
    uint32_t last_output_frames;
    bool output_sample_time_valid;
    uint64_t duplicate_cycles;
    uint64_t skipped_cycles;
    uint64_t interval_count;
    uint64_t interval_total_ns;
    uint64_t max_interval_ns;
    uint64_t max_callback_ns;
    uint64_t deadline_misses;
} NS6IOMetricsSnapshot;

void ns6_io_metrics_init(NS6IOMetrics *metrics);
void ns6_io_metrics_record(NS6IOMetrics *metrics, uint32_t client_id,
                           uint32_t frames, uint64_t cycle);
void ns6_io_metrics_record_timed(NS6IOMetrics *metrics, uint32_t client_id,
                                 uint32_t frames, uint64_t cycle,
                                 uint64_t start_ns, uint64_t callback_ns,
                                 uint64_t deadline_ns);
void ns6_io_metrics_record_output_timed(NS6IOMetrics *metrics,
                                        uint32_t client_id, uint32_t frames,
                                        uint64_t cycle, uint64_t output_sample_time,
                                        bool output_sample_time_valid,
                                        uint64_t start_ns, uint64_t callback_ns,
                                        uint64_t deadline_ns);
size_t ns6_io_metrics_take_snapshot(NS6IOMetrics *metrics,
                                    NS6IOMetricsSnapshot *snapshots,
                                    size_t capacity);

extern NS6IOMetrics ns6_io_metrics;

#endif
