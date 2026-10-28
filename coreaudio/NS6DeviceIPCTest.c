#include "NS6DeviceClient.h"
#include "NS6DeviceAuthorization.h"
#include "NS6DeviceClientRegistry.h"
#include "NS6USB.h"
#include "NS6Transport.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <mach/mach_vm.h>
#include <pthread.h>
#include <stdatomic.h>
#include <servers/bootstrap.h>

static bool fake_usb_ready;
static bool fake_usb_paused;
static uint32_t fake_startup_buffer_frames;
bool ns6_usb_is_ready(void) { return fake_usb_ready; }
bool ns6_usb_request_restart(void) { return fake_usb_ready; }
void ns6_usb_set_paused(bool paused) { fake_usb_paused = paused; }
void ns6_usb_reset_capture_session(void) {}
void ns6_usb_set_startup_buffer_frames(uint32_t frames) { fake_startup_buffer_frames = frames; }

typedef struct {
    mach_port_t port;
    mach_port_t memory_entry;
    NS6SharedMemory *memory;
    mach_vm_size_t mapped_size;
    uint64_t session_id;
    NS6DeviceClientRegistry clients;
    pthread_t thread;
} FakeService;

static void update_fake_audio_active(FakeService *service) {
    fake_usb_paused = !ns6_device_client_registry_any_audio_active(&service->clients);
}

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
        if (request.header.msgh_id == NS6_DEVICE_MSG_HELLO) {
            NS6DeviceHelloRequest *hello = (NS6DeviceHelloRequest *)&request;
            assert(ns6_device_client_registry_attach(&service->clients, hello->client_id, 0));
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
        } else if (request.header.msgh_id == NS6_DEVICE_MSG_SET_AUDIO_ACTIVE ||
                   request.header.msgh_id == NS6_DEVICE_MSG_SET_STARTUP_BUFFER ||
                   request.header.msgh_id == NS6_DEVICE_MSG_RELEASE_CLIENT) {
            NS6DeviceControlRequest *control = (NS6DeviceControlRequest *)&request;
            NS6DeviceControlReply reply = {0};
            reply.header.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_MOVE_SEND_ONCE, 0);
            reply.header.msgh_size = sizeof(reply);
            reply.header.msgh_remote_port = request.header.msgh_remote_port;
            reply.header.msgh_id = NS6_DEVICE_MSG_CONTROL_REPLY;
            reply.status = control->session_id == service->session_id ? KERN_SUCCESS : MIG_BAD_ID;
            if (reply.status == KERN_SUCCESS && request.header.msgh_id == NS6_DEVICE_MSG_SET_AUDIO_ACTIVE) {
                assert(ns6_device_client_registry_set_audio_active(&service->clients,
                                                                  control->client_id,
                                                                  control->value != 0));
                update_fake_audio_active(service);
            } else if (reply.status == KERN_SUCCESS &&
                       request.header.msgh_id == NS6_DEVICE_MSG_RELEASE_CLIENT) {
                assert(ns6_device_client_registry_detach(&service->clients, control->client_id));
                update_fake_audio_active(service);
            }
            (void)mach_msg(&reply.header, MACH_SEND_MSG, sizeof(reply), 0,
                           MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
        } else if (request.header.msgh_id == NS6_DEVICE_MSG_GET_STATE) {
            NS6DeviceControlRequest *control = (NS6DeviceControlRequest *)&request;
            struct {
                mach_msg_header_t header;
                kern_return_t status;
                NS6DeviceStateSnapshot snapshot;
            } reply = {0};
            reply.header.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_MOVE_SEND_ONCE, 0);
            reply.header.msgh_size = sizeof(reply);
            reply.header.msgh_remote_port = request.header.msgh_remote_port;
            reply.header.msgh_id = NS6_DEVICE_MSG_STATE_REPLY;
            reply.status = control->session_id == service->session_id ? KERN_SUCCESS : MIG_BAD_ID;
            (void)mach_msg(&reply.header, MACH_SEND_MSG, sizeof(reply), 0,
                           MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
        }
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
    assert(ns6_device_session_begin(&service->memory->session, session_id));
    fake_usb_paused = true;
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

static void test_multiple_hal_helpers_share_service_session(void) {
    FakeService persistent_service;
    fake_service_start(&persistent_service, 0xabc123u);

    NS6DeviceClient first_hal = {0};
    first_hal.service_port = MACH_PORT_NULL;
    assert(ns6_device_client_connect_to_port(&first_hal, persistent_service.port));
    uint64_t session = first_hal.session_id;
    assert(session == 0xabc123u);
    atomic_store(&first_hal.shared_memory->output.read_cursor, 77);
    assert(ns6_device_client_set_audio_active(&first_hal, true));
    assert(!fake_usb_paused);

    NS6DeviceClient second_hal = {0};
    second_hal.service_port = MACH_PORT_NULL;
    assert(ns6_device_client_connect_to_port(&second_hal, persistent_service.port));
    assert(second_hal.session_id == session);
    assert(ns6_device_session_is_current(&second_hal.shared_memory->session, session));
    assert(atomic_load(&second_hal.shared_memory->output.read_cursor) == 77);
    assert(ns6_device_client_get_state_result(&first_hal, &(NS6DeviceStateSnapshot){0}) == NS6_DEVICE_CLIENT_STATE_READY);
    assert(ns6_device_client_set_audio_active(&second_hal, true));
    assert(!fake_usb_paused);
    assert(ns6_device_client_set_audio_active(&first_hal, false));
    assert(!fake_usb_paused);
    assert(ns6_device_client_set_audio_active(&second_hal, false));
    assert(fake_usb_paused);
    assert(ns6_device_client_set_audio_active(&first_hal, true));
    assert(!fake_usb_paused);
    ns6_device_client_disconnect(&first_hal);
    assert(fake_usb_paused);
    ns6_device_client_disconnect(&second_hal);
    fake_service_stop(&persistent_service);
}

static void test_service_restart_reconnects_without_shared_stale_state(void) {
    FakeService first_service;
    fake_service_start(&first_service, 1234);
    NS6DeviceClient client = {0};
    client.service_port = MACH_PORT_NULL;
    assert(ns6_device_client_connect_to_port(&client, first_service.port));
    assert(client.connected && client.session_id == 1234);
    assert(client.shared_memory->protocol_version == NS6_SHARED_MEMORY_PROTOCOL_VERSION);
    first_service.memory->output.read_cursor = 8;
    ns6_device_client_disconnect(&client);
    fake_service_stop(&first_service);

    FakeService second_service;
    fake_service_start(&second_service, 5678);
    assert(ns6_device_client_connect_to_port(&client, second_service.port));
    assert(client.connected && client.session_id == 5678);
    assert(atomic_load(&client.shared_memory->output.read_cursor) == 0);
    ns6_device_client_disconnect(&client);
    fake_service_stop(&second_service);
}

int main(void) {
    assert(ns6_device_peer_uid_authorized(0, 202));
    assert(ns6_device_peer_uid_authorized(202, 202));
    assert(!ns6_device_peer_uid_authorized(501, 202));
    assert(!ns6_device_peer_uid_authorized(202, (uid_t)-1));
    NS6DeviceClientRegistry registry = {0};
    assert(ns6_device_client_registry_attach(&registry, 1, 101));
    assert(ns6_device_client_registry_attach(&registry, 1, 101));
    assert(ns6_device_client_registry_attach(&registry, 2, 202));
    assert(registry.clients[0].process_id == 101);
    assert(registry.clients[1].process_id == 202);
    assert(ns6_device_client_registry_set_audio_active(&registry, 1, true));
    assert(ns6_device_client_registry_set_audio_active(&registry, 2, true));
    assert(ns6_device_client_registry_set_audio_active(&registry, 1, false));
    assert(ns6_device_client_registry_any_audio_active(&registry));
    assert(ns6_device_client_registry_detach(&registry, 2));
    assert(!ns6_device_client_registry_any_audio_active(&registry));

    assert(ns6_device_client_registry_attach(&registry, 3, 303));
    assert(ns6_device_client_registry_attach(&registry, 4, 303));
    assert(ns6_device_client_registry_attach(&registry, 5, 404));
    assert(ns6_device_client_registry_set_audio_active(&registry, 3, true));
    assert(ns6_device_client_registry_set_audio_active(&registry, 5, true));
    assert(ns6_device_client_registry_detach_process(&registry, 303) == 2);
    assert(!ns6_device_client_registry_contains(&registry, 3));
    assert(!ns6_device_client_registry_contains(&registry, 4));
    assert(ns6_device_client_registry_contains(&registry, 5));
    assert(ns6_device_client_registry_any_audio_active(&registry));
    assert(ns6_device_client_registry_detach_process(&registry, 404) == 1);
    assert(!ns6_device_client_registry_any_audio_active(&registry));

    test_protocol_rejects_version_mismatch();
    test_client_disconnect_invalidates_mapping();
    test_service_restart_reconnects_without_shared_stale_state();
    test_multiple_hal_helpers_share_service_session();
    puts("NS6 device Mach IPC tests passed");
    return 0;
}
