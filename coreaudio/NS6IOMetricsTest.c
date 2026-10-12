#include "NS6IOMetrics.h"

#include <assert.h>
#include <stddef.h>

int main(void) {
    NS6IOMetrics metrics;
    NS6IOMetricsSnapshot snapshot[NS6_IO_METRICS_MAX_CLIENTS];
    ns6_io_metrics_init(&metrics);

    ns6_io_metrics_record(&metrics, 17, 96, 1001);
    ns6_io_metrics_record(&metrics, 17, 96, 1001);
    ns6_io_metrics_record(&metrics, 29, 128, 1001);
    ns6_io_metrics_record(&metrics, 17, 96, 1003);
    ns6_io_metrics_record(&metrics, 29, 128, 1004);

    size_t count = ns6_io_metrics_take_snapshot(&metrics, snapshot,
                                                NS6_IO_METRICS_MAX_CLIENTS);
    assert(count == 2);
    assert(snapshot[0].client_id == 17);
    assert(snapshot[0].calls == 3);
    assert(snapshot[0].frames == 288);
    assert(snapshot[0].last_cycle == 1003);
    assert(snapshot[0].duplicate_cycles == 1);
    assert(snapshot[0].skipped_cycles == 1);
    assert(snapshot[1].client_id == 29);
    assert(snapshot[1].calls == 2);
    assert(snapshot[1].frames == 256);
    assert(snapshot[1].last_cycle == 1004);
    assert(snapshot[1].duplicate_cycles == 0);
    assert(snapshot[1].skipped_cycles == 2);

    count = ns6_io_metrics_take_snapshot(&metrics, snapshot,
                                         NS6_IO_METRICS_MAX_CLIENTS);
    assert(count == 2);
    assert(snapshot[0].calls == 0 && snapshot[0].frames == 0);
    assert(snapshot[0].duplicate_cycles == 0);
    assert(snapshot[0].skipped_cycles == 0);

    ns6_io_metrics_record_timed(&metrics, 41, 96, 1, 1000000, 500000,
                                2176870);
    ns6_io_metrics_record_timed(&metrics, 41, 96, 2, 3200000, 2300000,
                                2176870);
    ns6_io_metrics_record_timed(&metrics, 41, 96, 3, 6000000, 1000000,
                                2176870);
    count = ns6_io_metrics_take_snapshot(&metrics, snapshot,
                                         NS6_IO_METRICS_MAX_CLIENTS);
    assert(count == 3);
    assert(snapshot[2].client_id == 41);
    assert(snapshot[2].calls == 3);
    assert(snapshot[2].interval_count == 2);
    assert(snapshot[2].interval_total_ns == 5000000);
    assert(snapshot[2].max_callback_ns == 2300000);
    assert(snapshot[2].max_interval_ns == 2800000);
    assert(snapshot[2].deadline_misses == 1);

    count = ns6_io_metrics_take_snapshot(&metrics, snapshot,
                                         NS6_IO_METRICS_MAX_CLIENTS);
    assert(count == 3);
    assert(snapshot[0].calls == 0 && snapshot[0].frames == 0);
    assert(snapshot[0].duplicate_cycles == 0);
    assert(snapshot[0].skipped_cycles == 0);
    assert(snapshot[2].max_callback_ns == 0);
    assert(snapshot[2].max_interval_ns == 0);
    assert(snapshot[2].deadline_misses == 0);
    return 0;
}
