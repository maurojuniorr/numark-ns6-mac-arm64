#include "NS6MIDI.h"
#include <arpa/inet.h>
#include <pthread.h>
#include <os/log.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define MIDI_PACKET_BYTES 42
#define MIDI_IDLE_BYTE 0xfd
#define MIDI_TERMINATOR 0x00
#define MIDI_READ_SLOTS 4
#define MIDI_WRITE_QUEUE 256
#define DRIVER_PORT 48238
#define BRIDGE_PORT 48239

struct read_slot { unsigned char data[MIDI_PACKET_BYTES]; };
struct write_packet { unsigned char data[MIDI_PACKET_BYTES]; };
static IOUSBInterfaceInterface **usb;
static UInt8 input_pipe,output_pipe;
static struct read_slot reads[MIDI_READ_SLOTS];
static struct write_packet queue[MIDI_WRITE_QUEUE];
static unsigned head,tail;
static pthread_mutex_t queue_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t queue_ready=PTHREAD_COND_INITIALIZER;
static pthread_t writer,receiver;
static bool writer_started,receiver_started;
static int inbound_socket=-1,outbound_socket=-1;
static struct sockaddr_in bridge_address;
static atomic_bool running;

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
static void publish_packet(const unsigned char *data,UInt32 length){for(UInt32 offset=0;offset+2<length;offset+=3){unsigned char status=data[offset];if(status==MIDI_IDLE_BYTE||status==MIDI_TERMINATOR||!(status&0x80))continue;forward_to_bridge(data+offset,3);}}
static void submit_read(struct read_slot *slot);
static void read_complete(void *reference,IOReturn status,void *argument){struct read_slot *slot=reference;if(status==kIOReturnSuccess){UInt32 length=(UInt32)(uintptr_t)argument;if(length>MIDI_PACKET_BYTES)length=MIDI_PACKET_BYTES;publish_packet(slot->data,length);}if(atomic_load(&running))submit_read(slot);}
static void submit_read(struct read_slot *slot){if((*usb)->ReadPipeAsync(usb,input_pipe,slot->data,sizeof(slot->data),read_complete,slot)!=kIOReturnSuccess&&atomic_load(&running))fprintf(stderr,"Numark NS6 MIDI input unavailable\n");}
static void *write_worker(void *unused){(void)unused;for(;;){struct write_packet packet;pthread_mutex_lock(&queue_lock);while(head==tail&&atomic_load(&running))pthread_cond_wait(&queue_ready,&queue_lock);if(head==tail&&!atomic_load(&running)){pthread_mutex_unlock(&queue_lock);break;}packet=queue[head];head=(head+1)%MIDI_WRITE_QUEUE;pthread_mutex_unlock(&queue_lock);IOReturn result=(*usb)->WritePipe(usb,output_pipe,packet.data,sizeof(packet.data));if(result!=kIOReturnSuccess)os_log_error(OS_LOG_DEFAULT,"Numark NS6 MIDI output failed: %{public}u",(unsigned)result);}return NULL;}
static void *receive_worker(void *unused){(void)unused;unsigned char data[41];while(atomic_load(&running)){ssize_t length=recv(inbound_socket,data,sizeof(data),0);if(length>0)enqueue(data,(unsigned)length);}return NULL;}
static bool open_sockets(void){
    inbound_socket=socket(AF_INET,SOCK_DGRAM,0);outbound_socket=socket(AF_INET,SOCK_DGRAM,0);if(inbound_socket<0||outbound_socket<0)return false;
    int reuse=1;setsockopt(inbound_socket,SOL_SOCKET,SO_REUSEADDR,&reuse,sizeof(reuse));struct sockaddr_in local={.sin_len=sizeof(local),.sin_family=AF_INET,.sin_port=htons(DRIVER_PORT),.sin_addr.s_addr=htonl(INADDR_LOOPBACK)};
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
    usb=new_usb;input_pipe=output_pipe=0;head=tail=0;writer_started=receiver_started=false;
    if(!find_pipes()||(*usb)->ClearPipeStallBothEnds(usb,input_pipe)!=kIOReturnSuccess||(*usb)->ClearPipeStallBothEnds(usb,output_pipe)!=kIOReturnSuccess||!open_sockets()){close_sockets();usb=NULL;return false;}
    atomic_store(&running,true);
    if(pthread_create(&writer,NULL,write_worker,NULL)!=0){atomic_store(&running,false);close_sockets();usb=NULL;return false;}
    writer_started=true;
    if(pthread_create(&receiver,NULL,receive_worker,NULL)!=0){ns6_midi_stop();return false;}
    receiver_started=true;
    for(unsigned i=0;i<MIDI_READ_SLOTS;++i)submit_read(&reads[i]);
    return true;
}
void ns6_midi_stop(void){
    bool was_running=atomic_exchange(&running,false);
    if(!was_running&&!writer_started&&!receiver_started){close_sockets();usb=NULL;return;}
    pthread_mutex_lock(&queue_lock);pthread_cond_broadcast(&queue_ready);pthread_mutex_unlock(&queue_lock);
    if(usb&&input_pipe)(*usb)->AbortPipe(usb,input_pipe);
    if(inbound_socket>=0){close(inbound_socket);inbound_socket=-1;}
    if(writer_started){pthread_join(writer,NULL);writer_started=false;}
    if(receiver_started){pthread_join(receiver,NULL);receiver_started=false;}
    if(outbound_socket>=0){close(outbound_socket);outbound_socket=-1;}
    usb=NULL;input_pipe=output_pipe=0;
}
