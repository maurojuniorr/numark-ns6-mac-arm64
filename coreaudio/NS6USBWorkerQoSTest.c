#include "NS6USBWorkerQoS.h"

#include <assert.h>

int main(void) {
    qos_class_t actual_class = QOS_CLASS_UNSPECIFIED;
    int relative_priority = 0;
    assert(ns6_usb_worker_set_interactive_qos(&actual_class,
                                               &relative_priority));
    assert(actual_class == QOS_CLASS_USER_INTERACTIVE);
    return 0;
}
