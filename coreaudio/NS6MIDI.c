#include "NS6MIDI.h"
#include <CoreMIDI/CoreMIDI.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <mach/mach_time.h>

#define MIDI_PACKET_BYTES 42
#define MIDI_IDLE_BYTE 0xfd
#define MIDI_TERMINATOR 0x00
#define MIDI_READ_SLOTS 4
#define MIDI_WRITE_QUEUE 256

struct midi_read_slot { unsigned char data[MIDI_PACKET_BYTES]; };
struct midi_write_packet { unsigned char data[MIDI_PACKET_BYTES]; };

static IOUSBInterfaceInterface **usb;
static UInt8 input_pipe,output_pipe;
static MIDIClientRef client;
static MIDIEndpointRef source,destination;
static struct midi_read_slot read_slots[MIDI_READ_SLOTS];
static struct midi_write_packet write_queue[MIDI_WRITE_QUEUE];
static unsigned write_head,write_tail;
static pthread_mutex_t write_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t write_available=PTHREAD_COND_INITIALIZER;
static pthread_t write_thread;
static atomic_bool running;

static bool find_pipes(void){
    UInt8 endpoints=0;
    if((*usb)->GetNumEndpoints(usb,&endpoints)!=kIOReturnSuccess)return false;
    for(UInt8 pipe=1;pipe<=endpoints;++pipe){
        UInt8 direction=0,number=0,type=0,interval=0; UInt16 maximum=0;
        if((*usb)->GetPipeProperties(usb,pipe,&direction,&number,&type,&maximum,&interval)!=kIOReturnSuccess)continue;
        if(direction==kUSBIn&&number==3&&type==kUSBBulk)input_pipe=pipe;
        if(direction==kUSBOut&&number==4&&type==kUSBBulk)output_pipe=pipe;
    }
    return input_pipe&&output_pipe;
}

static void publish_packet(const unsigned char *data,UInt32 length){
    unsigned char storage[1024];
    MIDIPacketList *list=(MIDIPacketList *)storage;
    MIDIPacket *cursor=MIDIPacketListInit(list);
    bool has_messages=false;
    UInt64 time=mach_absolute_time();
    for(UInt32 offset=0;offset+2<length;offset+=3){
        unsigned char status=data[offset];
        if(status==MIDI_IDLE_BYTE||status==MIDI_TERMINATOR)continue;
        if(!(status&0x80))continue;
        unsigned char message[3]={status,data[offset+1],data[offset+2]};
        MIDIPacket *next=MIDIPacketListAdd(list,sizeof(storage),cursor,time,sizeof(message),message);
        if(!next)break;
        cursor=next;
        has_messages=true;
    }
    if(has_messages)MIDIReceived(source,list);
}

static void submit_read(struct midi_read_slot *slot);
static void read_complete(void *reference,IOReturn status,void *argument){
    struct midi_read_slot *slot=reference;
    if(status==kIOReturnSuccess){
        UInt32 length=(UInt32)(uintptr_t)argument;
        if(length>MIDI_PACKET_BYTES)length=MIDI_PACKET_BYTES;
        if(length)publish_packet(slot->data,length);
    }
    if(atomic_load_explicit(&running,memory_order_acquire))submit_read(slot);
}
static void submit_read(struct midi_read_slot *slot){
    IOReturn result=(*usb)->ReadPipeAsync(usb,input_pipe,slot->data,sizeof(slot->data),read_complete,slot);
    if(result!=kIOReturnSuccess)fprintf(stderr,"Numark NS6 MIDI input submit failed: 0x%08x\n",result);
}

static void enqueue_packet(const unsigned char *packet){
    pthread_mutex_lock(&write_lock);
    unsigned next=(write_tail+1)%MIDI_WRITE_QUEUE;
    if(next!=write_head){memcpy(write_queue[write_tail].data,packet,MIDI_PACKET_BYTES);write_tail=next;pthread_cond_signal(&write_available);}
    pthread_mutex_unlock(&write_lock);
}
static void *write_worker(void *unused){
    (void)unused;
    for(;;){
        struct midi_write_packet packet;
        pthread_mutex_lock(&write_lock);
        while(write_head==write_tail&&atomic_load_explicit(&running,memory_order_acquire))pthread_cond_wait(&write_available,&write_lock);
        if(write_head==write_tail&&!atomic_load_explicit(&running,memory_order_acquire)){pthread_mutex_unlock(&write_lock);break;}
        packet=write_queue[write_head];write_head=(write_head+1)%MIDI_WRITE_QUEUE;
        pthread_mutex_unlock(&write_lock);
        IOReturn result=(*usb)->WritePipe(usb,output_pipe,packet.data,sizeof(packet.data));
        if(result!=kIOReturnSuccess&&atomic_load_explicit(&running,memory_order_acquire))fprintf(stderr,"Numark NS6 MIDI output failed: 0x%08x\n",result);
    }
    return NULL;
}

static unsigned midi_length(unsigned char status){
    switch(status&0xf0){case 0x80:case 0x90:case 0xa0:case 0xb0:case 0xe0:return 3;case 0xc0:case 0xd0:return 2;default:return 0;}
}
static void receive_from_core_midi(const MIDIPacketList *list,void *reference,void *connection){
    (void)reference;(void)connection;
    const MIDIPacket *packet=list->packet;
    for(UInt32 packet_index=0;packet_index<list->numPackets;++packet_index,packet=MIDIPacketNext(packet)){
        UInt16 offset=0;
        while(offset<packet->length){
            unsigned char status=packet->data[offset];
            if(status==0xf0){
                UInt16 remaining=packet->length-offset;
                UInt16 size=remaining<41?remaining:41;
                unsigned char out[MIDI_PACKET_BYTES];memset(out,MIDI_IDLE_BYTE,sizeof(out));memcpy(out,packet->data+offset,size);out[MIDI_PACKET_BYTES-1]=MIDI_TERMINATOR;enqueue_packet(out);break;
            }
            unsigned length=midi_length(status);
            if(!length||offset+length>packet->length){++offset;continue;}
            unsigned char out[MIDI_PACKET_BYTES];memset(out,MIDI_IDLE_BYTE,sizeof(out));memcpy(out,packet->data+offset,length);out[MIDI_PACKET_BYTES-1]=MIDI_TERMINATOR;enqueue_packet(out);offset+=length;
        }
    }
}

static void dispose_endpoints(void){
    if(source)MIDIEndpointDispose(source);source=0;
    if(destination)MIDIEndpointDispose(destination);destination=0;
    if(client)MIDIClientDispose(client);client=0;
}

bool ns6_midi_start(IOUSBInterfaceInterface **new_usb,CFRunLoopRef run_loop){
    if(atomic_load_explicit(&running,memory_order_acquire))return true;
    if(!new_usb||!run_loop)return false;
    usb=new_usb;input_pipe=0;output_pipe=0;write_head=0;write_tail=0;
    if(!find_pipes()){usb=NULL;return false;}
    if(MIDIClientCreate(CFSTR("Numark NS6"),NULL,NULL,&client)!=noErr){usb=NULL;return false;}
    if(MIDISourceCreate(client,CFSTR("Numark NS6 Controls"),&source)!=noErr||MIDIDestinationCreate(client,CFSTR("Numark NS6 LEDs"),receive_from_core_midi,NULL,&destination)!=noErr){dispose_endpoints();usb=NULL;return false;}
    atomic_store_explicit(&running,true,memory_order_release);
    if(pthread_create(&write_thread,NULL,write_worker,NULL)!=0){atomic_store(&running,false);dispose_endpoints();usb=NULL;return false;}
    for(unsigned i=0;i<MIDI_READ_SLOTS;++i)submit_read(&read_slots[i]);
    return true;
}

void ns6_midi_stop(void){
    if(!atomic_exchange_explicit(&running,false,memory_order_acq_rel))return;
    pthread_mutex_lock(&write_lock);pthread_cond_broadcast(&write_available);pthread_mutex_unlock(&write_lock);
    if(usb&&input_pipe)(*usb)->AbortPipe(usb,input_pipe);
    pthread_join(write_thread,NULL);
    dispose_endpoints();
    usb=NULL;input_pipe=0;output_pipe=0;
}
