#include "NS6DeviceClient.h"

#include <mach/mach_vm.h>
#include <servers/bootstrap.h>
#include <string.h>

static bool exchange_message(mach_port_t service_port, mach_msg_header_t *request,
                             mach_msg_size_t request_size, mach_msg_header_t *reply,
                             mach_msg_size_t reply_size) {
    mach_port_t reply_port = MACH_PORT_NULL;
    if (mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &reply_port) != KERN_SUCCESS)
        return false;
    request->msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, MACH_MSG_TYPE_MAKE_SEND_ONCE);
    request->msgh_remote_port = service_port;
    request->msgh_local_port = reply_port;
    request->msgh_size = request_size;
    kern_return_t result = mach_msg(request, MACH_SEND_MSG, request_size, 0,
                                   MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
    if (result == KERN_SUCCESS) {
        union { mach_msg_header_t alignment; uint8_t bytes[4096]; } receive_buffer;
        memset(&receive_buffer, 0, sizeof(receive_buffer));
        result = mach_msg(&receive_buffer.alignment, MACH_RCV_MSG, 0, sizeof(receive_buffer), reply_port,
                          MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
        if (result == KERN_SUCCESS) {
            if (receive_buffer.alignment.msgh_size > reply_size) result = MACH_RCV_TOO_LARGE;
            else memcpy(reply, &receive_buffer, receive_buffer.alignment.msgh_size);
        }
    }
    mach_port_mod_refs(mach_task_self(), reply_port, MACH_PORT_RIGHT_RECEIVE, -1);
    return result == KERN_SUCCESS;
}

bool ns6_device_client_connect(NS6DeviceClient *client) {
    if (!client) return false;
    mach_port_t port = MACH_PORT_NULL;
    if (bootstrap_look_up(bootstrap_port, NS6_DEVICE_SERVICE_NAME, &port) != KERN_SUCCESS)
        return false;
    bool connected = ns6_device_client_connect_to_port(client, port);
    if (mach_port_type(mach_task_self(), port, &(mach_port_type_t){0}) == KERN_SUCCESS)
        mach_port_deallocate(mach_task_self(), port);
    return connected;
}

bool ns6_device_client_connect_to_port(NS6DeviceClient *client, mach_port_t service_port) {
    if (!client || service_port == MACH_PORT_NULL) return false;
    ns6_device_client_disconnect(client);
    if (mach_port_mod_refs(mach_task_self(), service_port, MACH_PORT_RIGHT_SEND, 1) != KERN_SUCCESS)
        return false;

    NS6DeviceHelloRequest request = {0};
    request.header.msgh_id = NS6_DEVICE_MSG_HELLO;
    request.protocol_version = NS6_DEVICE_PROTOCOL_VERSION;
    request.client_struct_size = sizeof(NS6SharedMemory);
    NS6DeviceHelloReply reply = {0};
    if (!exchange_message(service_port, &request.header, sizeof(request), &reply.header, sizeof(reply))) {
        mach_port_deallocate(mach_task_self(), service_port);
        return false;
    }

    bool valid = reply.header.msgh_id == NS6_DEVICE_MSG_HELLO_REPLY &&
                 ns6_device_protocol_version_supported(reply.protocol_version) &&
                 reply.shared_struct_size == sizeof(NS6SharedMemory) &&
                 reply.status == KERN_SUCCESS && reply.memory_entry.type == MACH_MSG_PORT_DESCRIPTOR &&
                 reply.memory_entry.name != MACH_PORT_NULL && reply.mapped_size != 0 &&
                 reply.memory_offset < reply.mapped_size &&
                 sizeof(NS6SharedMemory) <= reply.mapped_size - reply.memory_offset;
    if (!valid) {
        if (reply.header.msgh_bits & MACH_MSGH_BITS_COMPLEX) {
            mach_port_deallocate(mach_task_self(), reply.memory_entry.name);
        }
        mach_port_deallocate(mach_task_self(), service_port);
        return false;
    }

    mach_vm_address_t address = 0;
    kern_return_t result = mach_vm_map(mach_task_self(), &address, reply.mapped_size, 0,
                                       VM_FLAGS_ANYWHERE, reply.memory_entry.name, 0, FALSE,
                                       VM_PROT_READ | VM_PROT_WRITE,
                                       VM_PROT_READ | VM_PROT_WRITE, VM_INHERIT_NONE);
    mach_port_deallocate(mach_task_self(), reply.memory_entry.name);
    if (result != KERN_SUCCESS) {
        mach_port_deallocate(mach_task_self(), service_port);
        return false;
    }

    NS6SharedMemory *memory = (NS6SharedMemory *)(uintptr_t)(address + reply.memory_offset);
    if (memory->protocol_version != NS6_SHARED_MEMORY_PROTOCOL_VERSION ||
        memory->struct_size != sizeof(*memory)) {
        mach_vm_deallocate(mach_task_self(), address, reply.mapped_size);
        mach_port_deallocate(mach_task_self(), service_port);
        return false;
    }
    client->service_port = service_port;
    client->mapping_address = address;
    client->mapping_size = reply.mapped_size;
    client->shared_memory = memory;
    client->session_id = reply.session_id;
    client->connected = true;
    return true;
}

void ns6_device_client_disconnect(NS6DeviceClient *client) {
    if (!client) return;
    if (client->mapping_address && client->mapping_size)
        mach_vm_deallocate(mach_task_self(), client->mapping_address, client->mapping_size);
    if (client->service_port != MACH_PORT_NULL)
        mach_port_deallocate(mach_task_self(), client->service_port);
    memset(client, 0, sizeof(*client));
    client->service_port = MACH_PORT_NULL;
}

static bool send_control(NS6DeviceClient *client, mach_msg_id_t message, uint32_t value) {
    if (!client || !client->connected || client->service_port == MACH_PORT_NULL) return false;
    NS6DeviceControlRequest request = {0};
    request.header.msgh_id = message;
    request.protocol_version = NS6_DEVICE_PROTOCOL_VERSION;
    request.value = value;
    NS6DeviceControlReply reply = {0};
    if (!exchange_message(client->service_port, &request.header, sizeof(request),
                          &reply.header, sizeof(reply))) return false;
    return reply.header.msgh_id == NS6_DEVICE_MSG_CONTROL_REPLY && reply.status == KERN_SUCCESS;
}

bool ns6_device_client_set_audio_active(NS6DeviceClient *client, bool active) {
    return send_control(client, NS6_DEVICE_MSG_SET_AUDIO_ACTIVE, active ? 1u : 0u);
}

bool ns6_device_client_set_startup_buffer(NS6DeviceClient *client, UInt32 frames) {
    return send_control(client, NS6_DEVICE_MSG_SET_STARTUP_BUFFER, frames);
}

bool ns6_device_client_get_state(NS6DeviceClient *client, NS6DeviceStateSnapshot *snapshot) {
    if (!client || !client->connected || !snapshot) return false;
    NS6DeviceControlRequest request = {0};
    request.header.msgh_id = NS6_DEVICE_MSG_GET_STATE;
    request.protocol_version = NS6_DEVICE_PROTOCOL_VERSION;
    NS6DeviceStateSnapshot reply_snapshot;
    struct {
        mach_msg_header_t header;
        kern_return_t status;
        NS6DeviceStateSnapshot snapshot;
    } reply = {0};
    if (!exchange_message(client->service_port, &request.header, sizeof(request), &reply.header, sizeof(reply)))
        return false;
    if (reply.header.msgh_id != NS6_DEVICE_MSG_STATE_REPLY || reply.status != KERN_SUCCESS) return false;
    reply_snapshot = reply.snapshot;
    *snapshot = reply_snapshot;
    return true;
}
