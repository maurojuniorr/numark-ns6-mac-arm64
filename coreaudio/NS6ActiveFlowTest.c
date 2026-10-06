#include "NS6ActiveFlow.h"
#include <assert.h>

int main(void) {
    NS6ActiveFlow flow = {0};
    NS6ActiveFlowSnapshot snapshot = ns6_active_flow_snapshot(&flow, 100, 20);
    assert(snapshot.frames == 0);

    ns6_active_flow_record(&flow, 512, 42, 100);
    snapshot = ns6_active_flow_snapshot(&flow, 101, 20);
    assert(snapshot.frames == 512 && snapshot.client_id == 42);

    snapshot = ns6_active_flow_snapshot(&flow, 121, 20);
    assert(snapshot.frames == 0 && snapshot.client_id == 0);

    ns6_active_flow_record(&flow, 256, 87, 200);
    snapshot = ns6_active_flow_snapshot(&flow, 201, 20);
    assert(snapshot.frames == 256 && snapshot.client_id == 87);

    ns6_active_flow_clear(&flow);
    snapshot = ns6_active_flow_snapshot(&flow, 202, 20);
    assert(snapshot.frames == 0);
    return 0;
}
