#include "NS6DeviceMachService.h"

#include "NS6DeviceProtocol.h"
#include "NS6DeviceAuthorization.h"
#include "NS6DeviceClientRegistry.h"
#include "NS6DeviceOwner.h"
#include "NS6USB.h"
#include "NS6Transport.h"

#include <mach/mach_vm.h>
#include <mach/mach_time.h>
#include <os/log.h>
#include <pthread.h>
#include <errno.h>
#include <pwd.h>
#include <signal.h>
#include <servers/bootstrap.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <bsm/libbsm.h>

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
    NS6DeviceClientRegistry clients;
    bool running;
    atomic_bool audio_active;
    uint32_t startup_buffer_frames;
    uint64_t session_id;
    uid_t coreaudiod_uid;
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

static void handle_hello(NS6DeviceHelloRequest *request, pid_t peer_pid) {
    NS6DeviceHelloReply reply = {0};
    reply.header.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_MOVE_SEND_ONCE, 0);
    reply.header.msgh_size = sizeof(reply);
    reply.header.msgh_remote_port = request->header.msgh_remote_port;
    reply.header.msgh_id = NS6_DEVICE_MSG_HELLO_REPLY;
    reply.protocol_version = NS6_DEVICE_PROTOCOL_VERSION;
    reply.shared_struct_size = sizeof(NS6SharedMemory);
    reply.status = KERN_SUCCESS;
    if (!ns6_device_protocol_version_supported(request->protocol_version) ||
        request->client_struct_size != sizeof(NS6SharedMemory) || request->client_id == 0) {
        os_log_error(OS_LOG_DEFAULT,
                     "NS6 Mach hello rejected: client protocol %u/struct %u, server protocol %u/struct %zu",
                     request->protocol_version, request->client_struct_size,
                     NS6_DEVICE_PROTOCOL_VERSION, sizeof(NS6SharedMemory));
        reply.status = MIG_BAD_ARGUMENTS;
        reply.mapped_size = 0;
        reply.memory_entry.name = MACH_PORT_NULL;
        reply.memory_entry.disposition = 0;
        reply.memory_entry.type = 0;
        (void)send_reply(&reply.header, sizeof(reply));
        return;
    }
    if (!ns6_device_client_registry_attach(&service.clients, request->client_id,
                                           (int32_t)peer_pid)) {
        os_log_error(OS_LOG_DEFAULT, "NS6 Mach hello rejected: client registry is full");
        reply.status = KERN_RESOURCE_SHORTAGE;
        reply.mapped_size = 0;
        reply.memory_entry.name = MACH_PORT_NULL;
        reply.memory_entry.disposition = 0;
        reply.memory_entry.type = 0;
        (void)send_reply(&reply.header, sizeof(reply));
        return;
    }
    reply.mapped_size = service.mapping_size;
    reply.memory_offset = service.memory_offset;
    reply.session_id = service.session_id;
    os_log(OS_LOG_DEFAULT, "NS6 Mach client %llu pid %d attached to persistent session %llu",
           (unsigned long long)request->client_id, peer_pid,
           (unsigned long long)service.session_id);
    reply.header.msgh_bits |= MACH_MSGH_BITS_COMPLEX;
    reply.body.msgh_descriptor_count = 1;
    reply.memory_entry.name = service.memory_entry;
    reply.memory_entry.disposition = MACH_MSG_TYPE_COPY_SEND;
    reply.memory_entry.type = MACH_MSG_PORT_DESCRIPTOR;
    (void)send_reply(&reply.header, sizeof(reply));
}

static void update_audio_active_from_clients(void) {
    bool active = ns6_device_client_registry_any_audio_active(&service.clients);
    bool was_active = atomic_exchange_explicit(&service.audio_active, active, memory_order_acq_rel);
    if (active) {
        if (!was_active) ns6_usb_set_paused(false);
    } else if (was_active) {
        ns6_usb_set_paused(true);
    }
}

static void log_active_clients(void) {
    char owners[256] = {0};
    size_t used = 0;
    for (size_t i = 0; i < NS6_DEVICE_MAX_CLIENTS; ++i) {
        const NS6DeviceClientLease *client = &service.clients.clients[i];
        if (client->client_id == 0 || !client->audio_active || used >= sizeof(owners)) continue;
        int written = snprintf(owners + used, sizeof(owners) - used, "%s%llu(pid=%d)",
                              used ? ", " : "", (unsigned long long)client->client_id,
                              client->process_id);
        if (written < 0) break;
        used += (size_t)written < sizeof(owners) - used ? (size_t)written : sizeof(owners) - used;
    }
    if (!used) strlcpy(owners, "none", sizeof(owners));
    os_log(OS_LOG_DEFAULT, "Numark NS6 active service leases: %{public}s", owners);
}

static void reap_dead_clients(void) {
    bool reaped = false;
    for (size_t i = 0; i < NS6_DEVICE_MAX_CLIENTS; ++i) {
        const NS6DeviceClientLease *client = &service.clients.clients[i];
        if (!client->client_id || client->process_id <= 0) continue;
        if (kill((pid_t)client->process_id, 0) == 0 || errno != ESRCH) continue;

        int32_t process_id = client->process_id;
        size_t detached = ns6_device_client_registry_detach_process(&service.clients,
                                                                    process_id);
        if (!detached) continue;
        reaped = true;
        os_log_error(OS_LOG_DEFAULT,
                     "Numark NS6 reaped %zu orphaned Mach client lease(s) for exited PID %d",
                     detached, process_id);
    }
    if (reaped) {
        update_audio_active_from_clients();
        log_active_clients();
    }
}

static void handle_control(NS6DeviceControlRequest *request, pid_t peer_pid) {
    if (!ns6_device_protocol_version_supported(request->protocol_version)) {
        reply_control(&request->header, MIG_BAD_ARGUMENTS);
        return;
    }
    if (!ns6_device_session_is_current(&service.memory->session, request->session_id) ||
        !ns6_device_client_registry_contains(&service.clients, request->client_id)) {
        reply_control(&request->header, MIG_BAD_ID);
        return;
    }
    kern_return_t status = KERN_SUCCESS;
    switch (request->header.msgh_id) {
        case NS6_DEVICE_MSG_SET_AUDIO_ACTIVE:
            if (request->value > 1) status = MIG_BAD_ARGUMENTS;
            else {
                bool active_before = atomic_load_explicit(&service.audio_active, memory_order_acquire);
                bool client_found = ns6_device_client_registry_set_audio_active(
                    &service.clients, request->client_id, request->value != 0);
                update_audio_active_from_clients();
                bool active_after = atomic_load_explicit(&service.audio_active, memory_order_acquire);
                os_log(OS_LOG_DEFAULT, "Numark NS6 audio-active request: service client %llu pid %d, requested %{public}s, registered %{public}s, aggregate %{public}s -> %{public}s",
                       (unsigned long long)request->client_id, peer_pid,
                       request->value ? "active" : "inactive",
                       client_found ? "yes" : "no", active_before ? "active" : "inactive",
                       active_after ? "active" : "inactive");
                log_active_clients();
            }
            break;
        case NS6_DEVICE_MSG_RELEASE_CLIENT:
            if (!ns6_device_client_registry_detach(&service.clients, request->client_id))
                status = MIG_BAD_ID;
            else {
                update_audio_active_from_clients();
                os_log(OS_LOG_DEFAULT, "NS6 Mach client %llu pid %d released",
                       (unsigned long long)request->client_id, peer_pid);
                log_active_clients();
            }
            break;
        case NS6_DEVICE_MSG_SET_STARTUP_BUFFER:
            if (!ns6_device_client_registry_contains(&service.clients, request->client_id))
                status = MIG_BAD_ID;
            else if (request->value == 0 || request->value > 65536) status = MIG_BAD_ARGUMENTS;
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

static void handle_state(NS6DeviceControlRequest *request) {
    struct {
        mach_msg_header_t header;
        kern_return_t status;
        NS6DeviceStateSnapshot snapshot;
    } reply = {0};
    reply.header.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_MOVE_SEND_ONCE, 0);
    reply.header.msgh_size = sizeof(reply);
    reply.header.msgh_remote_port = request->header.msgh_remote_port;
    reply.header.msgh_id = NS6_DEVICE_MSG_STATE_REPLY;
    reply.status = ns6_device_protocol_version_supported(request->protocol_version) &&
                   ns6_device_session_is_current(&service.memory->session, request->session_id) &&
                   ns6_device_client_registry_contains(&service.clients, request->client_id)
                       ? KERN_SUCCESS : MIG_BAD_ID;
    pthread_mutex_lock(&state_lock);
    reply.snapshot = service.snapshot;
    pthread_mutex_unlock(&state_lock);
    reply.snapshot.active_frames = service.startup_buffer_frames;
    (void)send_reply(&reply.header, sizeof(reply));
}

static void reject_unauthorized_request(mach_msg_header_t *request) {
    if (request->msgh_id == NS6_DEVICE_MSG_HELLO) {
        NS6DeviceHelloReply reply = {0};
        reply.header.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_MOVE_SEND_ONCE, 0);
        reply.header.msgh_size = sizeof(reply);
        reply.header.msgh_remote_port = request->msgh_remote_port;
        reply.header.msgh_id = NS6_DEVICE_MSG_HELLO_REPLY;
        reply.protocol_version = NS6_DEVICE_PROTOCOL_VERSION;
        reply.shared_struct_size = sizeof(NS6SharedMemory);
        reply.status = KERN_NO_ACCESS;
        (void)send_reply(&reply.header, sizeof(reply));
    } else if (request->msgh_id == NS6_DEVICE_MSG_GET_STATE) {
        struct {
            mach_msg_header_t header;
            kern_return_t status;
            NS6DeviceStateSnapshot snapshot;
        } reply = {0};
        reply.header.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_MOVE_SEND_ONCE, 0);
        reply.header.msgh_size = sizeof(reply);
        reply.header.msgh_remote_port = request->msgh_remote_port;
        reply.header.msgh_id = NS6_DEVICE_MSG_STATE_REPLY;
        reply.status = KERN_NO_ACCESS;
        (void)send_reply(&reply.header, sizeof(reply));
    } else {
        reply_control(request, KERN_NO_ACCESS);
    }
}

static bool sender_is_authorized(const uint8_t *message, mach_msg_size_t message_size,
                                uid_t coreaudiod_uid, pid_t *peer_pid) {
    mach_msg_size_t trailer_offset =
        (message_size + sizeof(natural_t) - 1u) & ~(sizeof(natural_t) - 1u);
    const mach_msg_audit_trailer_t *trailer =
        (const mach_msg_audit_trailer_t *)(const void *)(message + trailer_offset);
    if (trailer->msgh_trailer_type != MACH_MSG_TRAILER_FORMAT_0 ||
        trailer->msgh_trailer_size < sizeof(*trailer)) return false;
    uid_t peer_uid = audit_token_to_euid(trailer->msgh_audit);
    if (peer_pid) *peer_pid = audit_token_to_pid(trailer->msgh_audit);
    return ns6_device_peer_uid_authorized(peer_uid, coreaudiod_uid);
}

static void *service_thread(void *unused) {
    (void)unused;
    union {
        mach_msg_header_t header;
        uint8_t bytes[8192];
    } request;
    while (!atomic_load_explicit(&service.stopping, memory_order_acquire)) {
        memset(&request, 0, sizeof(request));
        kern_return_t result = mach_msg(&request.header,
                                       MACH_RCV_MSG | MACH_RCV_LARGE |
                                           MACH_RCV_TIMEOUT |
                                           MACH_RCV_TRAILER_TYPE(MACH_MSG_TRAILER_FORMAT_0) |
                                           MACH_RCV_TRAILER_ELEMENTS(MACH_RCV_TRAILER_AUDIT),
                                       0, sizeof(request), service.receive_port,
                                       1000, MACH_PORT_NULL);
        if (result != KERN_SUCCESS) {
            if (atomic_load_explicit(&service.stopping, memory_order_acquire)) break;
            if (result == MACH_RCV_TIMED_OUT) reap_dead_clients();
            continue;
        }
        pid_t peer_pid = 0;
        if (!sender_is_authorized(request.bytes, request.header.msgh_size,
                                  service.coreaudiod_uid, &peer_pid)) {
            mach_msg_size_t trailer_offset =
                (request.header.msgh_size + sizeof(natural_t) - 1u) & ~(sizeof(natural_t) - 1u);
            const mach_msg_audit_trailer_t *trailer =
                (const mach_msg_audit_trailer_t *)(const void *)(request.bytes + trailer_offset);
            os_log_error(OS_LOG_DEFAULT, "NS6 Mach service rejected unauthorized peer pid %d euid %u",
                         audit_token_to_pid(trailer->msgh_audit),
                         audit_token_to_euid(trailer->msgh_audit));
            reject_unauthorized_request(&request.header);
            continue;
        }
        reap_dead_clients();
        if (request.header.msgh_id == NS6_DEVICE_MSG_HELLO &&
            request.header.msgh_size >= sizeof(NS6DeviceHelloRequest)) {
            handle_hello((NS6DeviceHelloRequest *)&request, peer_pid);
        } else if (request.header.msgh_id == NS6_DEVICE_MSG_GET_STATE &&
                   request.header.msgh_size >= sizeof(NS6DeviceControlRequest)) {
            handle_state((NS6DeviceControlRequest *)&request);
        } else if (request.header.msgh_size >= sizeof(NS6DeviceControlRequest)) {
            handle_control((NS6DeviceControlRequest *)&request, peer_pid);
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
    struct passwd *coreaudiod_account = getpwnam("_coreaudiod");
    if (!coreaudiod_account) {
        os_log_error(OS_LOG_DEFAULT, "NS6 Mach service could not resolve the _coreaudiod account");
        return false;
    }
    service.coreaudiod_uid = coreaudiod_account->pw_uid;
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
    if (service.session_id == 0) service.session_id = 1;
    memset(&service.clients, 0, sizeof(service.clients));
    if (!ns6_device_session_begin(&memory->session, service.session_id)) {
        mach_port_mod_refs(mach_task_self(), service.receive_port, MACH_PORT_RIGHT_RECEIVE, -1);
        mach_port_deallocate(mach_task_self(), service.memory_entry);
        service.receive_port = MACH_PORT_NULL;
        service.memory_entry = MACH_PORT_NULL;
        service.memory = NULL;
        ns6_device_owner_release(&service.owner);
        return false;
    }
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
