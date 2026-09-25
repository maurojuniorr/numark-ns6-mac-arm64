#include <CoreMIDI/CoreMIDI.h>
#include <arpa/inet.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define DRIVER_PORT 48238
#define BRIDGE_PORT 48239

static volatile sig_atomic_t running=1;
static int socket_fd=-1;
static struct sockaddr_in driver_address;

static void stop(int signal_number){(void)signal_number;running=0;}
static unsigned midi_length(unsigned char status){
    switch(status&0xf0){case 0x80:case 0x90:case 0xa0:case 0xb0:case 0xe0:return 3;case 0xc0:case 0xd0:return 2;default:return 0;}
}
static void send_to_driver(const unsigned char *data,size_t length){
    if(socket_fd>=0&&length)sendto(socket_fd,data,length,0,(const struct sockaddr *)&driver_address,sizeof(driver_address));
}
static void receive_from_application(const MIDIPacketList *list,void *reference,void *connection){
    (void)reference;(void)connection;
    const MIDIPacket *packet=list->packet;
    for(UInt32 index=0;index<list->numPackets;++index,packet=MIDIPacketNext(packet)){
        UInt16 offset=0;
        while(offset<packet->length){
            unsigned char status=packet->data[offset];
            if(status==0xf0){
                UInt16 remaining=packet->length-offset;
                send_to_driver(packet->data+offset,remaining>41?41:remaining);
                break;
            }
            unsigned length=midi_length(status);
            if(!length||offset+length>packet->length){++offset;continue;}
            send_to_driver(packet->data+offset,length);
            offset+=length;
        }
    }
}
static bool open_socket(void){
    socket_fd=socket(AF_INET,SOCK_DGRAM,0);
    if(socket_fd<0)return false;
    int reuse=1;setsockopt(socket_fd,SOL_SOCKET,SO_REUSEADDR,&reuse,sizeof(reuse));
    struct sockaddr_in local={.sin_len=sizeof(local),.sin_family=AF_INET,.sin_port=htons(BRIDGE_PORT),.sin_addr.s_addr=htonl(INADDR_LOOPBACK)};
    if(bind(socket_fd,(const struct sockaddr *)&local,sizeof(local))<0){close(socket_fd);socket_fd=-1;return false;}
    struct timeval timeout={.tv_sec=1};setsockopt(socket_fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
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
        unsigned char data[64];ssize_t length=recv(socket_fd,data,sizeof(data),0);
        if(length<=0)continue;
        unsigned char storage[128];MIDIPacketList *list=(MIDIPacketList *)storage;
        MIDIPacket *packet=MIDIPacketListInit(list);
        if(MIDIPacketListAdd(list,sizeof(storage),packet,0,(UInt16)length,data))MIDIReceived(source,list);
    }
    MIDIEndpointDispose(destination);MIDIEndpointDispose(source);MIDIClientDispose(client);close(socket_fd);
    return 0;
}
