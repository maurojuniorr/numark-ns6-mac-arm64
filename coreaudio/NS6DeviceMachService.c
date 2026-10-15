#include "NS6DeviceMachService.h"

#include "NS6DeviceProtocol.h"
#include "NS6DeviceOwner.h"
#include "NS6USB.h"
#include "NS6Transport.h"

#include <mach/mach_vm.h>
#include <mach/mach_time.h>
#include <os/log.h>
#include <pthread.h>
#include <servers/bootstrap.h>
#include <stdatomic.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    NS6SharedMemory *memory;
    mach_port_t receive_port;
    mach_port_t memory_entry;
    mach_vm_address_t mapping_base;
    mach_vm_size_t mapping_size;
    mach_vm_size_t memory_offset;
    pthread_t thread;
    atomic_bool stopping;
    NS6DeviceOwner owner;
    bool running;
    atomic_bool audio_active;
    uint32_t startup_buffer_frames;
    uint64_t session_id;
    NS6DeviceStateSnapshot snapshot;
} NS6DeviceService;

static NS6DeviceService service = {
    .receive_port = MACH_PORT_NULL,
    .memory_entry = MACH_PORT_NULL,
    .startup_buffer_frames = 256
};
static pthread_mutex_t state_lock = PTHREAD_MUTEX_INITIALIZER;

static kern_return_t send_reply(mach_msg_header_t *reply, mach_msg_size_t size) {
    return mach_msg(reply, MACH_SEND_MSG, size, 0, MACH_PORT_NULL,
                    MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
}

static void reply_control(mach_msg_header_t *request, kern_return_t status) {
    NS6DeviceControlReply reply = {0};
    reply.header.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_MOVE_SEND_ONCE, 0);
    reply.header.msgh_size = sizeof(reply);
    reply.header.msgh_remote_port = request->msgh_remote_port;
    reply.header.msgh_id = NS6_DEVICE_MSG_CONTROL_REPLY;
    reply.status = status;
    (void)send_reply(&reply.header, sizeof(reply));
}

static void handle_hello(NS6DeviceHelloRequest *request) {
    NS6DeviceHelloReply reply = {0};
    reply.header.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_MOVE_SEND_ONCE, 0);
    reply.header.msgh_size = sizeof(reply);
    reply.header.msgh_remote_port = request->header.msgh_remote_port;
    reply.header.msgh_id = NS6_DEVICE_MSG_HELLO_REPLY;
    reply.protocol_version = NS6_DEVICE_PROTOCOL_VERSION;
    reply.shared_struct_size = sizeof(NS6SharedMemory);
    reply.mapped_size = service.mapping_size;
    reply.memory_offset = service.memory_offset;
    reply.session_id = service.session_id;
    reply.status = KERN_SUCCESS;
    if (!ns6_device_protocol_version_supported(request->protocol_version) ||
        request->client_struct_size != sizeof(NS6SharedMemory)) {
        reply.status = MIG_BAD_ARGUMENTS;
        reply.mapped_size = 0;
        reply.memory_entry.name = MACH_PORT_NULL;
        reply.memory_entry.disposition = 0;
        reply.memory_entry.type = 0;
        (void)send_reply(&reply.header, sizeof(reply));
        return;
    }
    reply.header.msgh_bits |= MACH_MSGH_BITS_COMPLEX;
    reply.body.msgh_descriptor_count = 1;
    reply.memory_entry.name = service.memory_entry;
    reply.memory_entry.disposition = MACH_MSG_TYPE_COPY_SEND;
    reply.memory_entry.type = MACH_MSG_PORT_DESCRIPTOR;
    (void)send_reply(&reply.header, sizeof(reply));
}

static void handle_control(NS6DeviceControlRequest *request) {
    if (!ns6_device_protocol_version_supported(request->protocol_version)) {
        reply_control(&request->header, MIG_BAD_ARGUMENTS);
        return;
    }
    kern_return_t status = KERN_SUCCESS;
    switch (request->header.msgh_id) {
        case NS6_DEVICE_MSG_SET_AUDIO_ACTIVE:
            if (request->value > 1) status = MIG_BAD_ARGUMENTS;
            else {
                atomic_store_explicit(&service.audio_active, request->value != 0, memory_order_release);
                if (request->value != 0) {
                    ns6_usb_set_paused(false);
                } else {
                    ns6_usb_set_paused(true);
                    ns6_transport_discard();
                }
            }
            break;
        case NS6_DEVICE_MSG_SET_STARTUP_BUFFER:
            if (request->value == 0 || request->value > 65536) status = MIG_BAD_ARGUMENTS;
            else {
                bool changed = service.startup_buffer_frames != request->value;
                service.startup_buffer_frames = request->value;
                ns6_usb_set_startup_buffer_frames(request->value);
                if (changed && ns6_usb_is_ready() && !ns6_usb_request_restart()) status = KERN_FAILURE;
            }
            break;
        default:
            status = MIG_BAD_ID;
            break;
    }
    reply_control(&request->header, status);
}

static void handle_state(mach_msg_header_t *request) {
    struct {
        mach_msg_header_t header;
        kern_return_t status;
        NS6DeviceStateSnapshot snapshot;
    } reply = {0};
    reply.header.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_MOVE_SEND_ONCE, 0);
    reply.header.msgh_size = sizeof(reply);
    reply.header.msgh_remote_port = request->msgh_remote_port;
    reply.header.msgh_id = NS6_DEVICE_MSG_STATE_REPLY;
    reply.status = KERN_SUCCESS;
    pthread_mutex_lock(&state_lock);
    reply.snapshot = service.snapshot;
    pthread_mutex_unlock(&state_lock);
    reply.snapshot.active_frames = service.startup_buffer_frames;
    (void)send_reply(&reply.header, sizeof(reply));
}

static void *service_thread(void *unused) {
    (void)unused;
    union {
        mach_msg_header_t header;
        uint8_t bytes[4096];
    } request;
    while (!atomic_load_explicit(&service.stopping, memory_order_acquire)) {
        memset(&request, 0, sizeof(request));
        kern_return_t result = mach_msg(&request.header, MACH_RCV_MSG | MACH_RCV_LARGE,
                                       0, sizeof(request), service.receive_port,
                                       MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
        if (result != KERN_SUCCESS) {
            if (atomic_load_explicit(&service.stopping, memory_order_acquire)) break;
            continue;
        }
        if (request.header.msgh_id == NS6_DEVICE_MSG_HELLO &&
            request.header.msgh_size >= sizeof(NS6DeviceHelloRequest)) {
            handle_hello((NS6DeviceHelloRequest *)&request);
        } else if (request.header.msgh_id == NS6_DEVICE_MSG_GET_STATE) {
            handle_state(&request.header);
        } else if (request.header.msgh_size >= sizeof(NS6DeviceControlRequest)) {
            handle_control((NS6DeviceControlRequest *)&request);
        } else {
            reply_control(&request.header, MIG_BAD_ARGUMENTS);
        }
    }
    return NULL;
}

bool ns6_device_service_publish(NS6SharedMemory *memory) {
    if (!memory || service.running ||
        memory->protocol_version != NS6_SHARED_MEMORY_PROTOCOL_VERSION ||
        memory->struct_size != sizeof(*memory)) return false;
    ns6_device_owner_init(&service.owner);
    if (!ns6_device_owner_claim(&service.owner)) return false;

    mach_vm_address_t address = (mach_vm_address_t)(uintptr_t)memory;
    mach_vm_address_t page_base = address & ~((mach_vm_address_t)vm_page_size - 1u);
    mach_vm_size_t offset = address - page_base;
    mach_vm_size_t length = (offset + sizeof(*memory) + vm_page_size - 1u) &
                            ~((mach_vm_size_t)vm_page_size - 1u);
    mach_vm_size_t entry_size = length;
    kern_return_t result = mach_make_memory_entry_64(mach_task_self(), &entry_size,
                                                     page_base, VM_PROT_READ | VM_PROT_WRITE,
                                                     &service.memory_entry, MACH_PORT_NULL);
    if (result != KERN_SUCCESS || entry_size < length) {
        ns6_device_owner_release(&service.owner);
        return false;
    }

    result = bootstrap_check_in(bootstrap_port, NS6_DEVICE_SERVICE_NAME, &service.receive_port);
    if (result != KERN_SUCCESS) {
        mach_port_deallocate(mach_task_self(), service.memory_entry);
        service.memory_entry = MACH_PORT_NULL;
        ns6_device_owner_release(&service.owner);
        return false;
    }
    service.memory = memory;
    service.mapping_base = page_base;
    service.mapping_size = length;
    service.memory_offset = offset;
    service.session_id = mach_absolute_time() ^ (uint64_t)(uintptr_t)memory;
    pthread_mutex_lock(&state_lock);
    service.snapshot.connected = 0;
    service.snapshot.transport_ready = 0;
    service.snapshot.firmware_version[0] = '\0';
    pthread_mutex_unlock(&state_lock);
    atomic_init(&service.stopping, false);
    atomic_init(&service.audio_active, false);
    if (pthread_create(&service.thread, NULL, service_thread, NULL) != 0) {
        mach_port_mod_refs(mach_task_self(), service.receive_port, MACH_PORT_RIGHT_RECEIVE, -1);
        mach_port_deallocate(mach_task_self(), service.memory_entry);
        service.receive_port = MACH_PORT_NULL;
        service.memory_entry = MACH_PORT_NULL;
        service.memory = NULL;
        ns6_device_owner_release(&service.owner);
        return false;
    }
    service.running = true;
    os_log(OS_LOG_DEFAULT, "Numark NS6 persistent USB service claimed by PID %d, protocol %u, session %llu",
           getpid(), NS6_DEVICE_PROTOCOL_VERSION, (unsigned long long)service.session_id);
    return true;
}

void ns6_device_service_update_device_state(bool connected, bool transport_ready,
                                           const char *firmware_version) {
    pthread_mutex_lock(&state_lock);
    service.snapshot.connected = connected ? 1u : 0u;
    service.snapshot.transport_ready = transport_ready ? 1u : 0u;
    if (firmware_version) {
        strlcpy(service.snapshot.firmware_version, firmware_version,
                sizeof(service.snapshot.firmware_version));
    } else {
        service.snapshot.firmware_version[0] = '\0';
    }
    pthread_mutex_unlock(&state_lock);
}

bool ns6_device_service_audio_active(void) {
    return atomic_load_explicit(&service.audio_active, memory_order_acquire);
}

void ns6_device_service_stop(void) {
    if (!service.running) return;
    atomic_store_explicit(&service.stopping, true, memory_order_release);
    mach_port_mod_refs(mach_task_self(), service.receive_port, MACH_PORT_RIGHT_RECEIVE, -1);
    (void)pthread_join(service.thread, NULL);
    mach_port_deallocate(mach_task_self(), service.memory_entry);
    service.receive_port = MACH_PORT_NULL;
    service.memory_entry = MACH_PORT_NULL;
    service.memory = NULL;
    service.mapping_base = 0;
    service.mapping_size = 0;
    service.memory_offset = 0;
    service.running = false;
    ns6_device_owner_release(&service.owner);
    os_log(OS_LOG_DEFAULT, "Numark NS6 persistent USB service released by PID %d", getpid());
}
