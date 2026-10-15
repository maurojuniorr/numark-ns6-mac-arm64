#ifndef NS6_DEVICE_CLIENT_H
#define NS6_DEVICE_CLIENT_H

#include "NS6DeviceProtocol.h"
#include "NS6SharedMemory.h"

#include <stdbool.h>

typedef struct {
    mach_port_t service_port;
    mach_vm_address_t mapping_address;
    mach_vm_size_t mapping_size;
    NS6SharedMemory *shared_memory;
    uint64_t session_id;
    bool connected;
} NS6DeviceClient;

typedef enum {
    NS6_DEVICE_CLIENT_STATE_UNAVAILABLE = 0,
    NS6_DEVICE_CLIENT_STATE_READY,
    NS6_DEVICE_CLIENT_STATE_REPLACED
} NS6DeviceClientStateResult;

bool ns6_device_client_connect(NS6DeviceClient *client);
bool ns6_device_client_connect_to_port(NS6DeviceClient *client, mach_port_t service_port);
void ns6_device_client_disconnect(NS6DeviceClient *client);
bool ns6_device_client_set_audio_active(NS6DeviceClient *client, bool active);
bool ns6_device_client_set_startup_buffer(NS6DeviceClient *client, UInt32 frames);
bool ns6_device_client_get_state(NS6DeviceClient *client, NS6DeviceStateSnapshot *snapshot);
NS6DeviceClientStateResult ns6_device_client_get_state_result(
    NS6DeviceClient *client, NS6DeviceStateSnapshot *snapshot);

#endif
