#include <CoreMIDI/CoreMIDI.h>
#include <CoreAudio/CoreAudio.h>
#include <os/log.h>
#include "NS6MIDILease.h"
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
#define HEARTBEAT_TIMEOUT_MS NS6_MIDI_LEASE_TIMEOUT_MS

static const unsigned char heartbeat_message[]={0xf0,0x7d,0x4e,0x53,0x36,0x48,0xf7};

static volatile sig_atomic_t running=1;
static int socket_fd=-1;
static struct sockaddr_in driver_address;
static _Atomic uint64_t last_heartbeat_ms;
static _Atomic uint64_t last_client_activity_ms;
static _Atomic uint64_t last_client_lease_message_ms;
static atomic_bool midi_client_active;
static AudioDeviceID held_audio_device=kAudioObjectUnknown;
static AudioDeviceIOProcID held_audio_proc;
static uint64_t next_audio_check_ms;

static void blackout_controller(void);
static void stop(int signal_number){(void)signal_number;running=0;}
static uint64_t monotonic_ms(void){struct timespec now;clock_gettime(CLOCK_MONOTONIC,&now);return (uint64_t)now.tv_sec*1000+(uint64_t)now.tv_nsec/1000000;}
static OSStatus silent_audio(AudioDeviceID device,const AudioTimeStamp *now,const AudioBufferList *input,const AudioTimeStamp *input_time,AudioBufferList *output,const AudioTimeStamp *output_time,void *context){
    (void)device;(void)now;(void)input;(void)input_time;(void)output_time;(void)context;
    if(output)for(UInt32 i=0;i<output->mNumberBuffers;++i)
        if(output->mBuffers[i].mData)memset(output->mBuffers[i].mData,0,output->mBuffers[i].mDataByteSize);
    return noErr;
}
static void release_audio_session(void){
    if(held_audio_proc){
        AudioDeviceStop(held_audio_device,held_audio_proc);
        AudioDeviceDestroyIOProcID(held_audio_device,held_audio_proc);
        os_log(OS_LOG_DEFAULT,"Numark NS6 bridge silent audio session released");
    }
    held_audio_proc=NULL;held_audio_device=kAudioObjectUnknown;
}
static void maintain_audio_session(void){
    uint64_t now=monotonic_ms();
    if(now<next_audio_check_ms)return;
    next_audio_check_ms=now+(held_audio_proc?1000:250);
    CFStringRef uid=CFSTR("io.github.maurojuniorr.numark-ns6.device");
    AudioDeviceID device=kAudioObjectUnknown;
    UInt32 size=sizeof(device);
    AudioObjectPropertyAddress property={kAudioHardwarePropertyTranslateUIDToDevice,kAudioObjectPropertyScopeGlobal,kAudioObjectPropertyElementMain};
    OSStatus status=AudioObjectGetPropertyData(kAudioObjectSystemObject,&property,sizeof(uid),&uid,&size,&device);
    if(status||device==kAudioObjectUnknown){release_audio_session();return;}
    if(held_audio_proc&&device!=held_audio_device)release_audio_session();
    UInt32 alive=0,active=0;
    property.mSelector=kAudioDevicePropertyDeviceIsAlive;size=sizeof(alive);
    if(AudioObjectGetPropertyData(device,&property,0,NULL,&size,&alive)||!alive){release_audio_session();return;}
    property.mSelector=kAudioDevicePropertyDeviceIsRunningSomewhere;size=sizeof(active);
    if(AudioObjectGetPropertyData(device,&property,0,NULL,&size,&active))return;
    if(held_audio_proc){if(!active)release_audio_session();return;}
    /* Start as soon as the device is alive so reconnects do not wait for an app. */
    AudioDeviceIOProcID proc=NULL;
    status=AudioDeviceCreateIOProcID(device,silent_audio,NULL,&proc);
    if(!status){
        status=AudioDeviceStart(device,proc);
        if(status)AudioDeviceDestroyIOProcID(device,proc);
    }
    if(status){
        next_audio_check_ms=now+5000;
        os_log_error(OS_LOG_DEFAULT,"Numark NS6 bridge silent audio session failed: %d",(int)status);
        return;
    }
    held_audio_device=device;held_audio_proc=proc;
    os_log(OS_LOG_DEFAULT,"Numark NS6 bridge silent audio session started for connected device");
}
static bool is_heartbeat(const unsigned char *data,size_t length){return length==sizeof(heartbeat_message)&&memcmp(data,heartbeat_message,sizeof(heartbeat_message))==0;}
static unsigned midi_length(unsigned char status){
    switch(status&0xf0){case 0x80:case 0x90:case 0xa0:case 0xb0:case 0xe0:return 3;case 0xc0:case 0xd0:return 2;default:return 0;}
}
static void send_to_driver(const unsigned char *data,size_t length){
    if(socket_fd>=0&&length)sendto(socket_fd,data,length,0,(const struct sockaddr *)&driver_address,sizeof(driver_address));
}
static void send_client_lease(bool active){
    const unsigned char message[NS6_MIDI_LEASE_MESSAGE_BYTES]={'N','S','6','L',active?1:0};
    send_to_driver(message,sizeof(message));
    atomic_store(&last_client_lease_message_ms,monotonic_ms());
}
static void set_client_active(bool active){
    if(atomic_exchange(&midi_client_active,active)==active)return;
    send_client_lease(active);
}
static void observe_client_activity(void){
    uint64_t now=monotonic_ms();
    atomic_store(&last_client_activity_ms,now);
    set_client_active(true);
    uint64_t last=atomic_load(&last_client_lease_message_ms);
    if(now>=last&&now-last>=500&&atomic_load(&midi_client_active))send_client_lease(true);
}
static void expire_client_lease(void){
    uint64_t now=monotonic_ms(),last=atomic_load(&last_client_activity_ms);
    if(!ns6_midi_lease_expired(atomic_load(&midi_client_active),last,now,NS6_MIDI_LEASE_TIMEOUT_MS))return;
    set_client_active(false);
    uint64_t heartbeat=atomic_load(&last_heartbeat_ms);
    if(heartbeat&&now>=heartbeat&&now-heartbeat>=HEARTBEAT_TIMEOUT_MS){
        blackout_controller();
        atomic_store(&last_heartbeat_ms,0);
    }
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
    if(list&&list->numPackets)observe_client_activity();
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
    fprintf(stderr,"Numark NS6 MIDI Bridge ready\n");
    while(running){
        maintain_audio_session();
        unsigned char data[64];ssize_t length=recv(socket_fd,data,sizeof(data),0);
        if(length<=0){
            expire_client_lease();
            continue;
        }
        unsigned char storage[128];MIDIPacketList *list=(MIDIPacketList *)storage;
        MIDIPacket *packet=MIDIPacketListInit(list);
        if(MIDIPacketListAdd(list,sizeof(storage),packet,0,(UInt16)length,data))MIDIReceived(source,list);
        expire_client_lease();
    }
    set_client_active(false);
    release_audio_session();
    MIDIEndpointDispose(destination);MIDIEndpointDispose(source);MIDIClientDispose(client);close(socket_fd);
    return 0;
}
