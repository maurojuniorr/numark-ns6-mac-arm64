#ifndef NS6_USB_WORKER_QOS_H
#define NS6_USB_WORKER_QOS_H

#include <pthread.h>
#include <pthread/qos.h>
#include <stdbool.h>

static inline bool ns6_usb_worker_set_interactive_qos(qos_class_t *actual_class,
                                                      int *relative_priority) {
    if (!actual_class || !relative_priority) return false;
    if (pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0) != 0)
        return false;
    if (pthread_get_qos_class_np(pthread_self(), actual_class,
                                 relative_priority) != 0)
        return false;
    return *actual_class == QOS_CLASS_USER_INTERACTIVE;
}

#endif
