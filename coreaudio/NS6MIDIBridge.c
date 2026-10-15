#include <CoreMIDI/CoreMIDI.h>
#if defined(NS6_DEVICE_SERVICE_BUILD) && NS6_DEVICE_SERVICE_BUILD
#include "NS6DeviceMachService.h"
#include "NS6SharedMemory.h"
#include "NS6Transport.h"
#include "NS6USB.h"
#include <mach/mach_vm.h>
#include <os/log.h>
#endif
#include <arpa/inet.h>
#include <signal.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>


#define DRIVER_PORT 48238
#define BRIDGE_PORT 48239
#define MIDI_SOCKET_BUFFER (1024 * 1024)
#define HEARTBEAT_TIMEOUT_MS 2000

static const unsigned char heartbeat_message[]={0xf0,0x7d,0x4e,0x53,0x36,0x48,0xf7};

static volatile sig_atomic_t running=1;
static int socket_fd=-1;
static struct sockaddr_in driver_address;
static _Atomic uint64_t last_heartbeat_ms;
#if defined(NS6_DEVICE_SERVICE_BUILD) && NS6_DEVICE_SERVICE_BUILD
static NS6SharedMemory *shared_memory;
static mach_vm_address_t shared_memory_address;
static mach_vm_size_t shared_memory_allocation_size;
static NS6TransportReader transport_reader;
static uint64_t next_usb_retry_ms;
static bool usb_state_known;
static bool last_reported_usb_ready;
static uint64_t monotonic_ms(void);

static bool start_device_service(void) {
    shared_memory_allocation_size = (sizeof(*shared_memory) + vm_page_size - 1u) &
                                    ~((mach_vm_size_t)vm_page_size - 1u);
    if (mach_vm_allocate(mach_task_self(), &shared_memory_address,
                         shared_memory_allocation_size, VM_FLAGS_ANYWHERE) != KERN_SUCCESS)
        return false;
    shared_memory = (NS6SharedMemory *)(uintptr_t)shared_memory_address;
    memset(shared_memory, 0, shared_memory_allocation_size);
    ns6_shared_memory_init(shared_memory);
    if (!ns6_shared_memory_is_lock_free(shared_memory) ||
        !ns6_device_service_publish(shared_memory)) {
        mach_vm_deallocate(mach_task_self(), shared_memory_address, shared_memory_allocation_size);
        shared_memory = NULL;
        shared_memory_address = 0;
        shared_memory_allocation_size = 0;
        return false;
    }
    ns6_transport_bind_shared(&shared_memory->output, &transport_reader);
    next_usb_retry_ms = 0;
    usb_state_known = false;
    last_reported_usb_ready = false;
    return true;
}

static void update_device_service_state(bool ready) {
    char firmware[32] = {0};
    if (ready) (void)ns6_usb_get_firmware_version(firmware);
    ns6_device_service_update_device_state(ready, ready, firmware);
    if (!usb_state_known || ready != last_reported_usb_ready) {
        os_log(OS_LOG_DEFAULT, "Numark NS6 service PID %d transport %{public}s, shared PCM ring %zu bytes",
               getpid(), ready ? "ready" : "waiting for USB", sizeof(NS6SharedMemory));
        usb_state_known = true;
        last_reported_usb_ready = ready;
    }
}

static void maintain_device_service(void) {
    uint64_t now = monotonic_ms();
    if (ns6_usb_is_ready()) {
        if (!usb_state_known || !last_reported_usb_ready) update_device_service_state(true);
        return;
    }
    if (usb_state_known && last_reported_usb_ready) update_device_service_state(false);
    if (now < next_usb_retry_ms) return;
    next_usb_retry_ms = now + 2000;
    bool ready = ns6_usb_start();
    update_device_service_state(ready);
}
#endif

static void stop(int signal_number){(void)signal_number;running=0;}
static uint64_t monotonic_ms(void){struct timespec now;clock_gettime(CLOCK_MONOTONIC,&now);return (uint64_t)now.tv_sec*1000+(uint64_t)now.tv_nsec/1000000;}
static bool is_heartbeat(const unsigned char *data,size_t length){return length==sizeof(heartbeat_message)&&memcmp(data,heartbeat_message,sizeof(heartbeat_message))==0;}
static unsigned midi_length(unsigned char status){
    switch(status&0xf0){case 0x80:case 0x90:case 0xa0:case 0xb0:case 0xe0:return 3;case 0xc0:case 0xd0:return 2;default:return 0;}
}
static void send_to_driver(const unsigned char *data,size_t length){
    if(socket_fd>=0&&length)sendto(socket_fd,data,length,0,(const struct sockaddr *)&driver_address,sizeof(driver_address));
}
static void blackout_controller(void){
    unsigned char report[39];
    size_t used=0;
    for(unsigned channel=0;channel<=4;++channel){
        for(unsigned cc=0;cc<=0x51;++cc){report[used++]=0xb0+channel;report[used++]=cc;report[used++]=0;if(used==sizeof(report)){send_to_driver(report,used);used=0;}}
        for(unsigned note=0;note<=0x50;++note){report[used++]=0x80+channel;report[used++]=note;report[used++]=0;if(used==sizeof(report)){send_to_driver(report,used);used=0;}}
    }
    if(used)send_to_driver(report,used);
}
static void receive_from_application(const MIDIPacketList *list,void *reference,void *connection){
    (void)reference;(void)connection;
    unsigned char report[41];
    size_t used=0;
    const MIDIPacket *packet=list->packet;
    for(UInt32 index=0;index<list->numPackets;++index,packet=MIDIPacketNext(packet)){
        UInt16 offset=0;
        while(offset<packet->length){
            unsigned char status=packet->data[offset];
            if(status==0xf0){
                /* Preserve ordering: drain any short MIDI messages first. */
                send_to_driver(report,used);
                used=0;
                UInt16 remaining=packet->length-offset;
                if(is_heartbeat(packet->data+offset,remaining)){
                    atomic_store(&last_heartbeat_ms,monotonic_ms());
                    break;
                }
                send_to_driver(packet->data+offset,remaining>41?41:remaining);
                break;
            }
            unsigned length=midi_length(status);
            if(!length||offset+length>packet->length){++offset;continue;}
            /* The NS6 accepts up to 41 bytes of MIDI payload in its fixed
               42-byte USB report. Batching prevents the close-time LED
               blackout from overflowing the driver's write queue. */
            if(used+length>sizeof(report)){
                send_to_driver(report,used);
                used=0;
            }
            memcpy(report+used,packet->data+offset,length);
            used+=length;
            offset+=length;
        }
    }
    send_to_driver(report,used);
}
static bool open_socket(void){
    socket_fd=socket(AF_INET,SOCK_DGRAM,0);
    if(socket_fd<0)return false;
    int reuse=1;setsockopt(socket_fd,SOL_SOCKET,SO_REUSEADDR,&reuse,sizeof(reuse));setsockopt(socket_fd,SOL_SOCKET,SO_RCVBUF,&(int){MIDI_SOCKET_BUFFER},sizeof(int));
    struct sockaddr_in local={.sin_len=sizeof(local),.sin_family=AF_INET,.sin_port=htons(BRIDGE_PORT),.sin_addr.s_addr=htonl(INADDR_LOOPBACK)};
    if(bind(socket_fd,(const struct sockaddr *)&local,sizeof(local))<0){close(socket_fd);socket_fd=-1;return false;}
    struct timeval timeout={.tv_usec=20000};
    if(setsockopt(socket_fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout))<0){close(socket_fd);socket_fd=-1;return false;}
    driver_address=(struct sockaddr_in){.sin_len=sizeof(driver_address),.sin_family=AF_INET,.sin_port=htons(DRIVER_PORT),.sin_addr.s_addr=htonl(INADDR_LOOPBACK)};
    return true;
}
int main(void){
    MIDIClientRef client=0;MIDIEndpointRef source=0,destination=0;
    signal(SIGINT,stop);signal(SIGTERM,stop);
    if(!open_socket()||MIDIClientCreate(CFSTR("Numark NS6 MIDI Bridge"),NULL,NULL,&client)!=noErr||MIDISourceCreate(client,CFSTR("Numark NS6"),&source)!=noErr||MIDIDestinationCreate(client,CFSTR("Numark NS6"),receive_from_application,NULL,&destination)!=noErr){fprintf(stderr,"Could not start Numark NS6 MIDI Bridge\n");if(destination)MIDIEndpointDispose(destination);if(source)MIDIEndpointDispose(source);if(client)MIDIClientDispose(client);if(socket_fd>=0)close(socket_fd);return 1;}
    /* Virtual endpoints do not inherit USB descriptors. Publish the NS6
       identity so host applications can associate their controller profile. */
    MIDIObjectSetStringProperty(source,kMIDIPropertyManufacturer,CFSTR("Numark"));
    MIDIObjectSetStringProperty(source,kMIDIPropertyModel,CFSTR("NS6"));
    MIDIObjectSetStringProperty(source,kMIDIPropertyDisplayName,CFSTR("Numark NS6 MIDI"));
    MIDIObjectSetIntegerProperty(source,kMIDIPropertyUniqueID,0x4e533601);
    MIDIObjectSetStringProperty(destination,kMIDIPropertyManufacturer,CFSTR("Numark"));
    MIDIObjectSetStringProperty(destination,kMIDIPropertyModel,CFSTR("NS6"));
    MIDIObjectSetStringProperty(destination,kMIDIPropertyDisplayName,CFSTR("Numark NS6 MIDI"));
    MIDIObjectSetIntegerProperty(destination,kMIDIPropertyUniqueID,0x4e533602);
#if defined(NS6_DEVICE_SERVICE_BUILD) && NS6_DEVICE_SERVICE_BUILD
    if (!start_device_service()) {
        os_log_error(OS_LOG_DEFAULT, "Numark NS6 device service could not publish shared memory / Mach endpoint");
        MIDIEndpointDispose(destination); MIDIEndpointDispose(source); MIDIClientDispose(client); close(socket_fd);
        return 1;
    }
    os_log(OS_LOG_DEFAULT, "Numark NS6 device service PID %d starting sole USB transport worker", getpid());
    maintain_device_service();
#endif
    fprintf(stderr,"Numark NS6 MIDI Bridge ready\n");
    while(running){
        unsigned char data[64];ssize_t length=recv(socket_fd,data,sizeof(data),0);
        if(length<=0){
            uint64_t heartbeat=atomic_load(&last_heartbeat_ms);
            if(heartbeat&&monotonic_ms()-heartbeat>HEARTBEAT_TIMEOUT_MS){
                blackout_controller();
                atomic_store(&last_heartbeat_ms,0);
            }
#if defined(NS6_DEVICE_SERVICE_BUILD) && NS6_DEVICE_SERVICE_BUILD
            maintain_device_service();
#endif
            continue;
        }
        unsigned char storage[128];MIDIPacketList *list=(MIDIPacketList *)storage;
        MIDIPacket *packet=MIDIPacketListInit(list);
        if(MIDIPacketListAdd(list,sizeof(storage),packet,0,(UInt16)length,data))MIDIReceived(source,list);
#if defined(NS6_DEVICE_SERVICE_BUILD) && NS6_DEVICE_SERVICE_BUILD
        maintain_device_service();
#endif
    }
#if defined(NS6_DEVICE_SERVICE_BUILD) && NS6_DEVICE_SERVICE_BUILD
    os_log(OS_LOG_DEFAULT, "Numark NS6 device service PID %d stopping USB transport", getpid());
    ns6_usb_stop();
    update_device_service_state(false);
    ns6_transport_bind_shared(NULL, NULL);
    ns6_device_service_stop();
    if (shared_memory_address)
        mach_vm_deallocate(mach_task_self(), shared_memory_address, shared_memory_allocation_size);
#endif
    MIDIEndpointDispose(destination);MIDIEndpointDispose(source);MIDIClientDispose(client);close(socket_fd);
    return 0;
}
