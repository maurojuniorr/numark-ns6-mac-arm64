#ifndef NS6_DEVICE_MACH_SERVICE_H
#define NS6_DEVICE_MACH_SERVICE_H

#include "NS6SharedMemory.h"

#include <stdbool.h>

bool ns6_device_service_publish(NS6SharedMemory *memory);
void ns6_device_service_stop(void);

#endif
