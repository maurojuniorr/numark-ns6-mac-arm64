#include "NS6MIDI.h"
#include "NS6MIDIParser.h"
#include <arpa/inet.h>
#include <IOKit/usb/USB.h>
#include <pthread.h>
#include <os/log.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define MIDI_PACKET_BYTES 42
#define MIDI_IDLE_BYTE 0xfd
#define MIDI_TERMINATOR 0x00
#define MIDI_READ_SLOTS 4
#define MIDI_WRITE_QUEUE 1024
#define MIDI_SOCKET_BUFFER (1024 * 1024)
#define DRIVER_PORT 48238
#define BRIDGE_PORT 48239

struct read_slot { unsigned char data[MIDI_PACKET_BYTES]; };
struct write_packet { unsigned char data[MIDI_PACKET_BYTES]; };
static IOUSBInterfaceInterface **usb;
static UInt8 input_pipe,output_pipe;
static struct read_slot reads[MIDI_READ_SLOTS];
static struct ns6_midi_parser parser;
static pthread_mutex_t parser_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t handshake_lock=PTHREAD_MUTEX_INITIALIZER;
static bool handshake_capture,handshake_reply_seen,handshake_sysex_active;
static unsigned char handshake_sysex[64];
static size_t handshake_sysex_length;
static struct write_packet queue[MIDI_WRITE_QUEUE];
static unsigned head,tail;
static pthread_mutex_t queue_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t pipe_recovery_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t queue_ready=PTHREAD_COND_INITIALIZER;
static pthread_t writer,receiver;
static bool writer_started,receiver_started;
static int inbound_socket=-1,outbound_socket=-1;
static struct sockaddr_in bridge_address;
static atomic_bool running;
static atomic_uint_fast64_t last_input_error_log_ms;

static uint64_t monotonic_ms(void){struct timespec now;clock_gettime(CLOCK_MONOTONIC,&now);return (uint64_t)now.tv_sec*1000+(uint64_t)now.tv_nsec/1000000;}
static bool recover_stalled_pipe(UInt8 pipe,const char *direction){
    if(!usb||!pipe||!atomic_load(&running))return false;
    pthread_mutex_lock(&pipe_recovery_lock);
    IOReturn result=(*usb)->ClearPipeStallBothEnds(usb,pipe);
    pthread_mutex_unlock(&pipe_recovery_lock);
    if(result==kIOReturnSuccess){
        os_log_error(OS_LOG_DEFAULT,"Numark NS6 MIDI %{public}s pipe stalled; endpoint cleared, resuming",direction);
        return true;
    }
    os_log_error(OS_LOG_DEFAULT,"Numark NS6 MIDI %{public}s pipe stall recovery failed: %{public}u",direction,(unsigned)result);
    return false;
}
static void log_input_error(IOReturn result,const char *stage){
    uint64_t now=monotonic_ms(),last=atomic_load(&last_input_error_log_ms);
    if(now-last>=1000&&atomic_compare_exchange_strong(&last_input_error_log_ms,&last,now))
        os_log_error(OS_LOG_DEFAULT,"Numark NS6 MIDI input %{public}s failed: %{public}u",stage,(unsigned)result);
}

static bool find_pipes(void){
    UInt8 endpoints=0;if((*usb)->GetNumEndpoints(usb,&endpoints)!=kIOReturnSuccess)return false;
    for(UInt8 pipe=1;pipe<=endpoints;++pipe){UInt8 direction=0,number=0,type=0,interval=0;UInt16 maximum=0;if((*usb)->GetPipeProperties(usb,pipe,&direction,&number,&type,&maximum,&interval)!=kIOReturnSuccess)continue;if(direction==kUSBIn&&number==3&&type==kUSBBulk)input_pipe=pipe;if(direction==kUSBOut&&number==4&&type==kUSBBulk)output_pipe=pipe;}
    return input_pipe&&output_pipe;
}
static void enqueue(const unsigned char *data,unsigned length){
    if(!length||length>=MIDI_PACKET_BYTES)return;
    pthread_mutex_lock(&queue_lock);unsigned next=(tail+1)%MIDI_WRITE_QUEUE;
    if(next!=head){memset(queue[tail].data,MIDI_IDLE_BYTE,MIDI_PACKET_BYTES);memcpy(queue[tail].data,data,length);queue[tail].data[MIDI_PACKET_BYTES-1]=MIDI_TERMINATOR;tail=next;pthread_cond_signal(&queue_ready);}pthread_mutex_unlock(&queue_lock);
}
static void forward_to_bridge(const unsigned char *data,unsigned length){if(outbound_socket>=0)sendto(outbound_socket,data,length,0,(const struct sockaddr *)&bridge_address,sizeof(bridge_address));}
static void inspect_handshake_reply(const unsigned char *data,UInt32 length){
    pthread_mutex_lock(&handshake_lock);
    if(!handshake_capture){pthread_mutex_unlock(&handshake_lock);return;}
    for(UInt32 i=0;i<length;++i){
        unsigned char byte=data[i];
        if(byte==MIDI_IDLE_BYTE)continue;
        if(byte==0xf0){handshake_sysex_active=true;handshake_sysex_length=0;handshake_sysex[handshake_sysex_length++]=byte;continue;}
        if(!handshake_sysex_active)continue;
        if(handshake_sysex_length<sizeof(handshake_sysex))handshake_sysex[handshake_sysex_length++]=byte;
        else{handshake_sysex_active=false;handshake_sysex_length=0;continue;}
        if(byte==0xf7){
            static const unsigned char prefix[]={0xf0,0x00,0x01,0x3f,0x00,0x79,0x51};
            if(handshake_sysex_length>=sizeof(prefix)&&!memcmp(handshake_sysex,prefix,sizeof(prefix))){
                handshake_reply_seen=true;
            }
            handshake_sysex_active=false;handshake_sysex_length=0;
        }else if(byte&0x80){handshake_sysex_active=false;handshake_sysex_length=0;}
    }
    pthread_mutex_unlock(&handshake_lock);
}
/*
 * The NS6 bulk endpoint delivers fixed 42-byte reports containing MIDI
 * triplets and idle bytes. Assemble across report boundaries, then forward
 * the complete messages in one compact datagram. Sending every triplet as a
 * separate UDP datagram made a busy jog stream capable of overtaking or
 * dropping a button Note Off, leaving CUE held in the host application.
 */
static void publish_packet(const unsigned char *data,UInt32 length){
    unsigned char messages[MIDI_PACKET_BYTES+3];
    size_t stream_length=ns6_midi_report_stream_length(length);
    pthread_mutex_lock(&parser_lock);
    inspect_handshake_reply(data,(UInt32)stream_length);
    size_t used=ns6_midi_parser_feed(&parser,data,stream_length,messages,sizeof(messages));
    if(used)forward_to_bridge(messages,(unsigned)used);
    pthread_mutex_unlock(&parser_lock);
}
static void submit_read(struct read_slot *slot);
static void read_complete(void *reference,IOReturn status,void *argument){
    struct read_slot *slot=reference;
    if(status==kIOReturnSuccess){UInt32 length=(UInt32)(uintptr_t)argument;if(length>MIDI_PACKET_BYTES)length=MIDI_PACKET_BYTES;publish_packet(slot->data,length);}
    else if(atomic_load(&running)){
        log_input_error(status,"completion");
        if(status==kIOUSBPipeStalled)recover_stalled_pipe(input_pipe,"input");
    }
    if(atomic_load(&running))submit_read(slot);
}
static void submit_read(struct read_slot *slot){
    if(!atomic_load(&running))return;
    IOReturn result=(*usb)->ReadPipeAsync(usb,input_pipe,slot->data,sizeof(slot->data),read_complete,slot);
    if(result==kIOReturnSuccess)return;
    log_input_error(result,"submission");
    if(result==kIOUSBPipeStalled&&recover_stalled_pipe(input_pipe,"input")){
        result=(*usb)->ReadPipeAsync(usb,input_pipe,slot->data,sizeof(slot->data),read_complete,slot);
        if(result==kIOReturnSuccess)return;
        log_input_error(result,"resubmission");
    }
}
static void *write_worker(void *unused){
    (void)unused;
    for(;;){
        struct write_packet packet;
        pthread_mutex_lock(&queue_lock);
        while(head==tail&&atomic_load(&running))pthread_cond_wait(&queue_ready,&queue_lock);
        if(head==tail&&!atomic_load(&running)){pthread_mutex_unlock(&queue_lock);break;}
        packet=queue[head];head=(head+1)%MIDI_WRITE_QUEUE;pthread_mutex_unlock(&queue_lock);
        IOReturn result=(*usb)->WritePipe(usb,output_pipe,packet.data,sizeof(packet.data));
        if(result==kIOUSBPipeStalled&&recover_stalled_pipe(output_pipe,"output"))
            result=(*usb)->WritePipe(usb,output_pipe,packet.data,sizeof(packet.data));
        if(result!=kIOReturnSuccess)os_log_error(OS_LOG_DEFAULT,"Numark NS6 MIDI output failed: %{public}u",(unsigned)result);
    }
    return NULL;
}
static void blackout_controller(void){
    if(!usb||!output_pipe)return;
    unsigned char packet[MIDI_PACKET_BYTES];
    const struct timespec settle={.tv_nsec=2000000};
    for(unsigned pass=0;pass<2;++pass){
        unsigned used=0;
        memset(packet,MIDI_IDLE_BYTE,sizeof(packet));
        for(unsigned channel=0;channel<=4;++channel){
            for(unsigned cc=0;cc<=0x51;++cc){
                packet[used++]=0xb0+channel;packet[used++]=cc;packet[used++]=0;
                if(used==39){packet[41]=MIDI_TERMINATOR;(*usb)->WritePipe(usb,output_pipe,packet,sizeof(packet));nanosleep(&settle,NULL);memset(packet,MIDI_IDLE_BYTE,sizeof(packet));used=0;}
            }
            for(unsigned note=0;note<=0x50;++note){
                packet[used++]=0x80+channel;packet[used++]=note;packet[used++]=0;
                if(used==39){packet[41]=MIDI_TERMINATOR;(*usb)->WritePipe(usb,output_pipe,packet,sizeof(packet));nanosleep(&settle,NULL);memset(packet,MIDI_IDLE_BYTE,sizeof(packet));used=0;}
            }
        }
        if(used){packet[41]=MIDI_TERMINATOR;(*usb)->WritePipe(usb,output_pipe,packet,sizeof(packet));nanosleep(&settle,NULL);}
    }
}
static void *receive_worker(void *unused){(void)unused;unsigned char data[41];while(atomic_load(&running)){ssize_t length=recv(inbound_socket,data,sizeof(data),0);if(length>0)enqueue(data,(unsigned)length);}return NULL;}
static bool open_sockets(void){
    inbound_socket=socket(AF_INET,SOCK_DGRAM,0);outbound_socket=socket(AF_INET,SOCK_DGRAM,0);if(inbound_socket<0||outbound_socket<0)return false;
    int reuse=1;setsockopt(inbound_socket,SOL_SOCKET,SO_REUSEADDR,&reuse,sizeof(reuse));setsockopt(inbound_socket,SOL_SOCKET,SO_RCVBUF,&(int){MIDI_SOCKET_BUFFER},sizeof(int));setsockopt(outbound_socket,SOL_SOCKET,SO_SNDBUF,&(int){MIDI_SOCKET_BUFFER},sizeof(int));struct sockaddr_in local={.sin_len=sizeof(local),.sin_family=AF_INET,.sin_port=htons(DRIVER_PORT),.sin_addr.s_addr=htonl(INADDR_LOOPBACK)};
    if(bind(inbound_socket,(struct sockaddr *)&local,sizeof(local))<0)return false;
    struct timeval timeout={.tv_sec=1};setsockopt(inbound_socket,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));bridge_address=(struct sockaddr_in){.sin_len=sizeof(bridge_address),.sin_family=AF_INET,.sin_port=htons(BRIDGE_PORT),.sin_addr.s_addr=htonl(INADDR_LOOPBACK)};return true;
}
static void close_sockets(void){
    if(inbound_socket>=0){close(inbound_socket);inbound_socket=-1;}
    if(outbound_socket>=0){close(outbound_socket);outbound_socket=-1;}
}
bool ns6_midi_start(IOUSBInterfaceInterface **new_usb,CFRunLoopRef run_loop){
    if(atomic_load(&running))return true;
    if(!new_usb||!run_loop)return false;
    usb=new_usb;input_pipe=output_pipe=0;head=tail=0;memset(&parser,0,sizeof(parser));writer_started=receiver_started=false;
    if(!find_pipes()||(*usb)->ClearPipeStallBothEnds(usb,input_pipe)!=kIOReturnSuccess||(*usb)->ClearPipeStallBothEnds(usb,output_pipe)!=kIOReturnSuccess||!open_sockets()){close_sockets();usb=NULL;return false;}
    atomic_store(&running,true);
    if(pthread_create(&writer,NULL,write_worker,NULL)!=0){atomic_store(&running,false);close_sockets();usb=NULL;return false;}
    writer_started=true;
    if(pthread_create(&receiver,NULL,receive_worker,NULL)!=0){ns6_midi_stop();return false;}
    receiver_started=true;
    for(unsigned i=0;i<MIDI_READ_SLOTS;++i)submit_read(&reads[i]);
    return true;
}
static void pump_until(CFRunLoopRef loop,uint64_t deadline){
    (void)loop;
    while(monotonic_ms()<deadline)CFRunLoopRunInMode(kCFRunLoopDefaultMode,0.005,true);
}
bool ns6_midi_handshake(CFRunLoopRef loop){
    if(!atomic_load(&running)||!loop)return false;
    unsigned char identify[31]={0xf0,0x00,0x01,0x3f,0x7f,0x79,0x50,0x00,0x10,0x04,0x01,0x00,0x00,0x00};
    for(unsigned i=14;i<30;++i)identify[i]=(unsigned char)(arc4random()&0x0f);
    identify[30]=0xf7;
    pthread_mutex_lock(&handshake_lock);
    handshake_capture=true;handshake_reply_seen=false;handshake_sysex_active=false;handshake_sysex_length=0;
    pthread_mutex_unlock(&handshake_lock);
    uint64_t started=monotonic_ms();
    enqueue(identify,sizeof(identify));
    uint64_t reply_deadline=started+100;
    while(monotonic_ms()<reply_deadline){
        pthread_mutex_lock(&handshake_lock);bool received=handshake_reply_seen;pthread_mutex_unlock(&handshake_lock);
        if(received)break;
        CFRunLoopRunInMode(kCFRunLoopDefaultMode,0.005,true);
    }
    pthread_mutex_lock(&handshake_lock);bool received=handshake_reply_seen;pthread_mutex_unlock(&handshake_lock);
    if(!received)os_log_error(OS_LOG_DEFAULT,"Numark NS6 host-identify sent but device 0x51 reply was not observed");
    /* The official sequence holds this gap for about 795 ms after host 0x50. */
    pump_until(loop,started+795);
    const unsigned char jog_enable[]={0xb1,0x3b,0x01,0xb2,0x3b,0x01,0xb3,0x3b,0x01,0xb4,0x3b,0x01};
    static const unsigned char wake[]={0xf0,0x00,0x01,0x3f,0x7f,0x79,0x60,0x00,0x01,0x49,0x01,0x00,0x00,0x00,0x00,0xf7};
    enqueue(jog_enable,sizeof(jog_enable));
    pump_until(loop,started+796);
    enqueue(wake,sizeof(wake));
    pthread_mutex_lock(&handshake_lock);handshake_capture=false;handshake_sysex_active=false;pthread_mutex_unlock(&handshake_lock);
    os_log(OS_LOG_DEFAULT,"Numark NS6 host-identify handshake completed; device reply %{public}s",received?"observed":"not observed");
    return received;
}
static void stop_midi(bool blackout){
    bool was_running=atomic_exchange(&running,false);
    if(!was_running&&!writer_started&&!receiver_started){close_sockets();usb=NULL;return;}
    pthread_mutex_lock(&queue_lock);pthread_cond_broadcast(&queue_ready);pthread_mutex_unlock(&queue_lock);
    if(usb&&input_pipe)(*usb)->AbortPipe(usb,input_pipe);
    if(inbound_socket>=0){close(inbound_socket);inbound_socket=-1;}
    if(writer_started){pthread_join(writer,NULL);writer_started=false;}
    /* The HAL still owns the USB interface here. Do the blackout before the
       audio shutdown releases it; Mixxx may already have destroyed its MIDI
       endpoint and cannot be relied on to call the mapping shutdown hook. */
    if(blackout)blackout_controller();
    if(receiver_started){pthread_join(receiver,NULL);receiver_started=false;}
    if(outbound_socket>=0){close(outbound_socket);outbound_socket=-1;}
    usb=NULL;input_pipe=output_pipe=0;
}
void ns6_midi_stop(void){stop_midi(true);}
void ns6_midi_stop_for_audio_restart(void){stop_midi(false);}
