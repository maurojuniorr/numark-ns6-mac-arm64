#include "NS6BulkINIdlePolicy.h"

#include <assert.h>

int main(void) {
    assert(ns6_bulk_in_reads_should_run(false, false));
    assert(ns6_bulk_in_reads_should_run(true, true));
#if NS6_KEEP_WAVEFORM_DRAIN_IDLE
    assert(ns6_bulk_in_reads_should_run(true, false));
#else
    assert(!ns6_bulk_in_reads_should_run(true, false));
#endif
    return 0;
}
