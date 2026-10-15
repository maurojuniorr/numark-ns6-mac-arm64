#ifndef NS6_DEVICE_PROTOCOL_H
#define NS6_DEVICE_PROTOCOL_H

#include <mach/mach.h>
#include <stdint.h>

#define NS6_DEVICE_SERVICE_NAME "io.github.maurojuniorr.numark-ns6-device-service"
#define NS6_DEVICE_PROTOCOL_VERSION 2u

enum {
    NS6_DEVICE_MSG_HELLO = 0x4e530001,
    NS6_DEVICE_MSG_HELLO_REPLY = 0x4e530002,
    NS6_DEVICE_MSG_SET_AUDIO_ACTIVE = 0x4e530003,
    NS6_DEVICE_MSG_SET_STARTUP_BUFFER = 0x4e530004,
    NS6_DEVICE_MSG_GET_STATE = 0x4e530005,
    NS6_DEVICE_MSG_CONTROL_REPLY = 0x4e530006,
    NS6_DEVICE_MSG_STATE_REPLY = 0x4e530007
};

typedef struct {
    mach_msg_header_t header;
    uint32_t protocol_version;
    uint32_t client_struct_size;
} NS6DeviceHelloRequest;

typedef struct {
    mach_msg_header_t header;
    mach_msg_body_t body;
    mach_msg_port_descriptor_t memory_entry;
    uint32_t protocol_version;
    uint32_t shared_struct_size;
    uint64_t mapped_size;
    uint64_t memory_offset;
    uint64_t session_id;
    kern_return_t status;
} NS6DeviceHelloReply;

typedef struct {
    mach_msg_header_t header;
    uint32_t protocol_version;
    uint32_t value;
} NS6DeviceControlRequest;

typedef struct {
    mach_msg_header_t header;
    kern_return_t status;
} NS6DeviceControlReply;

typedef struct {
    uint32_t connected;
    uint32_t transport_ready;
    char firmware_version[32];
    uint64_t underruns;
    uint64_t transfer_errors;
    uint64_t packet_errors;
    uint64_t clock_rate_millihz;
    uint32_t active_frames;
} NS6DeviceStateSnapshot;

static inline int ns6_device_protocol_version_supported(uint32_t version) {
    return version == NS6_DEVICE_PROTOCOL_VERSION;
}

#endif
