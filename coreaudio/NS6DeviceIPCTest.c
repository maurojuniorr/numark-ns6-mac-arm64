#include "NS6DeviceClient.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <mach/mach_vm.h>
#include <pthread.h>
#include <stdatomic.h>
#include <servers/bootstrap.h>

typedef struct {
    mach_port_t port;
    mach_port_t memory_entry;
    NS6SharedMemory *memory;
    mach_vm_size_t mapped_size;
    uint64_t session_id;
    pthread_t thread;
} FakeService;

static void *fake_service_thread(void *opaque) {
    FakeService *service = opaque;
    union { mach_msg_header_t header; uint8_t bytes[1024]; } request;
    for (;;) {
        memset(&request, 0, sizeof(request));
        kern_return_t result = mach_msg(&request.header, MACH_RCV_MSG,
                                       0, sizeof(request), service->port,
                                       MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
        if (result != KERN_SUCCESS) break;
        if (request.header.msgh_id == 99) break;
        assert(request.header.msgh_id == NS6_DEVICE_MSG_HELLO);
        NS6DeviceHelloRequest *hello = (NS6DeviceHelloRequest *)&request;
        NS6DeviceHelloReply reply = {0};
        reply.header.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_MOVE_SEND_ONCE, 0) | MACH_MSGH_BITS_COMPLEX;
        reply.header.msgh_size = sizeof(reply);
        reply.header.msgh_remote_port = request.header.msgh_remote_port;
        reply.header.msgh_id = NS6_DEVICE_MSG_HELLO_REPLY;
        reply.body.msgh_descriptor_count = 1;
        reply.memory_entry.name = service->memory_entry;
        reply.memory_entry.disposition = MACH_MSG_TYPE_COPY_SEND;
        reply.memory_entry.type = MACH_MSG_PORT_DESCRIPTOR;
        reply.protocol_version = hello->protocol_version;
        reply.shared_struct_size = sizeof(NS6SharedMemory);
        reply.mapped_size = service->mapped_size;
        reply.memory_offset = (uintptr_t)service->memory & (vm_page_size - 1u);
        reply.session_id = service->session_id;
        reply.status = KERN_SUCCESS;
        (void)mach_msg(&reply.header, MACH_SEND_MSG, sizeof(reply), 0,
                       MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
    }
    return NULL;
}

static void fake_service_start(FakeService *service, uint64_t session_id) {
    memset(service, 0, sizeof(*service));
    service->mapped_size = (sizeof(NS6SharedMemory) + vm_page_size - 1u) & ~(vm_page_size - 1u);
    mach_vm_address_t address = 0;
    assert(mach_vm_allocate(mach_task_self(), &address, service->mapped_size, VM_FLAGS_ANYWHERE) == KERN_SUCCESS);
    service->memory = (NS6SharedMemory *)(uintptr_t)address;
    memset(service->memory, 0, service->mapped_size);
    ns6_shared_memory_init(service->memory);
    service->session_id = session_id;
    assert(mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &service->port) == KERN_SUCCESS);
    assert(mach_port_insert_right(mach_task_self(), service->port, service->port,
                                  MACH_MSG_TYPE_MAKE_SEND) == KERN_SUCCESS);
    mach_vm_size_t entry_size = service->mapped_size;
    assert(mach_make_memory_entry_64(mach_task_self(), &entry_size, address,
                                     VM_PROT_READ | VM_PROT_WRITE, &service->memory_entry,
                                     MACH_PORT_NULL) == KERN_SUCCESS);
    assert(pthread_create(&service->thread, NULL, fake_service_thread, service) == 0);
}

static void fake_service_stop(FakeService *service) {
    mach_msg_header_t stop = {0};
    stop.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
    stop.msgh_size = sizeof(stop);
    stop.msgh_remote_port = service->port;
    stop.msgh_id = 99;
    assert(mach_msg(&stop, MACH_SEND_MSG, sizeof(stop), 0, MACH_PORT_NULL,
                    MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL) == KERN_SUCCESS);
    assert(pthread_join(service->thread, NULL) == 0);
    mach_port_deallocate(mach_task_self(), service->memory_entry);
    mach_port_deallocate(mach_task_self(), service->port);
    mach_port_mod_refs(mach_task_self(), service->port, MACH_PORT_RIGHT_RECEIVE, -1);
    mach_vm_deallocate(mach_task_self(), (mach_vm_address_t)(uintptr_t)service->memory,
                       service->mapped_size);
}

static void test_protocol_rejects_version_mismatch(void) {
    assert(ns6_device_protocol_version_supported(NS6_DEVICE_PROTOCOL_VERSION));
    assert(!ns6_device_protocol_version_supported(NS6_DEVICE_PROTOCOL_VERSION + 1));
    assert(!ns6_device_protocol_version_supported(0));
}

static void test_client_disconnect_invalidates_mapping(void) {
    mach_vm_address_t address = 0;
    assert(mach_vm_allocate(mach_task_self(), &address, vm_page_size, VM_FLAGS_ANYWHERE) == KERN_SUCCESS);
    NS6DeviceClient client;
    memset(&client, 0, sizeof(client));
    client.service_port = MACH_PORT_NULL;
    client.mapping_address = address;
    client.mapping_size = vm_page_size;
    client.shared_memory = (NS6SharedMemory *)(uintptr_t)address;
    client.connected = true;
    client.session_id = 42;

    ns6_device_client_disconnect(&client);

    assert(!client.connected);
    assert(client.shared_memory == NULL);
    assert(client.mapping_address == 0);
    assert(client.mapping_size == 0);
    assert(client.session_id == 0);
}

static void test_service_restart_reconnects_without_shared_stale_state(void) {
    FakeService first_service;
    fake_service_start(&first_service, 1234);
    NS6DeviceClient client = {0};
    client.service_port = MACH_PORT_NULL;
    assert(ns6_device_client_connect_to_port(&client, first_service.port));
    assert(client.connected && client.session_id == 1234);
    assert(client.shared_memory->protocol_version == NS6_SHARED_MEMORY_PROTOCOL_VERSION);
    first_service.memory->output.high_water = 8;
    ns6_device_client_disconnect(&client);
    fake_service_stop(&first_service);

    FakeService second_service;
    fake_service_start(&second_service, 5678);
    assert(ns6_device_client_connect_to_port(&client, second_service.port));
    assert(client.connected && client.session_id == 5678);
    assert(atomic_load(&client.shared_memory->output.high_water) == 0);
    ns6_device_client_disconnect(&client);
    fake_service_stop(&second_service);
}

int main(void) {
    test_protocol_rejects_version_mismatch();
    test_client_disconnect_invalidates_mapping();
    test_service_restart_reconnects_without_shared_stale_state();
    puts("NS6 device Mach IPC tests passed");
    return 0;
}
