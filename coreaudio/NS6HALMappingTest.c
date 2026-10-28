#include "NS6HALMapping.h"

#include <assert.h>
#include <pthread.h>
#include <stdio.h>

static NS6HALMapping mapping;
static atomic_bool withdraw_finished;

static void *withdraw_thread(void *unused) {
    (void)unused;
    ns6_hal_mapping_withdraw(&mapping);
    atomic_store_explicit(&withdraw_finished, true, memory_order_release);
    return NULL;
}

int main(void) {
    int first = 1, second = 2;
    NS6HALMappingLease lease;
    ns6_hal_mapping_init(&mapping);
    atomic_init(&withdraw_finished, false);
    assert(!ns6_hal_mapping_acquire(&mapping, &lease));

    ns6_hal_mapping_publish(&mapping, &first, 11);
    assert(ns6_hal_mapping_acquire(&mapping, &lease));
    assert(lease.memory == &first && lease.session_id == 11);

    pthread_t writer;
    assert(pthread_create(&writer, NULL, withdraw_thread, NULL) == 0);
    while (atomic_load_explicit(&mapping.memory, memory_order_acquire) != NULL) {}
    assert(!atomic_load_explicit(&withdraw_finished, memory_order_acquire));
    assert(!ns6_hal_mapping_acquire(&mapping, &(NS6HALMappingLease){0}));
    ns6_hal_mapping_release(&mapping);
    assert(pthread_join(writer, NULL) == 0);
    assert(atomic_load_explicit(&withdraw_finished, memory_order_acquire));

    ns6_hal_mapping_publish(&mapping, &second, 22);
    assert(ns6_hal_mapping_acquire(&mapping, &lease));
    assert(lease.memory == &second && lease.session_id == 22);
    ns6_hal_mapping_release(&mapping);
    puts("NS6 HAL mapping publication/reclamation passed");
    return 0;
}
