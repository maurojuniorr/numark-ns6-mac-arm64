#include "NS6MIDI.h"
#include "NS6MIDIParser.h"
#include "NS6MIDIRecovery.h"
#include "NS6MIDIReadLifecycle.h"
#include "NS6USBConfig.h"
#include <arpa/inet.h>
#include <IOKit/usb/USB.h>
#include <pthread.h>
#include <stddef.h>
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
static uint32_t pending_read_recovery_mask;
static uint32_t pending_read_clear_mask;
static atomic_uint_fast64_t read_recovery_requested_us[MIDI_READ_SLOTS];
static atomic_uint_fast64_t input_error_counts[MIDI_READ_SLOTS][3];
static atomic_uint_fast64_t last_input_error_log_ms;
static atomic_uint read_in_flight_mask;
static atomic_uint read_in_flight_minimum_count;
static atomic_bool read_in_flight_minimum_tracking;
static atomic_bool input_pipe_recovery_active;
static atomic_bool audio_client_active;
static atomic_uint read_target_count;
static atomic_uint_fast64_t read_slot_out_of_flight_since_ms[MIDI_READ_SLOTS];
static pthread_mutex_t queue_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t pipe_recovery_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t queue_ready=PTHREAD_COND_INITIALIZER;
static pthread_t writer,receiver;
static bool writer_started,receiver_started;
static int inbound_socket=-1,outbound_socket=-1;
static struct sockaddr_in bridge_address;
static atomic_bool running;

static uint64_t monotonic_ms(void){struct timespec now;clock_gettime(CLOCK_MONOTONIC,&now);return (uint64_t)now.tv_sec*1000+(uint64_t)now.tv_nsec/1000000;}
static uint64_t monotonic_us(void){struct timespec now;clock_gettime(CLOCK_MONOTONIC,&now);return (uint64_t)now.tv_sec*1000000+(uint64_t)now.tv_nsec/1000;}
static uint32_t read_slot_bit(const struct read_slot *slot){ptrdiff_t index=slot?slot-reads:-1;return index>=0&&index<MIDI_READ_SLOTS?UINT32_C(1)<<(unsigned)index:0;}
static uint32_t read_in_flight_snapshot(void){return atomic_load_explicit(&read_in_flight_mask,memory_order_acquire);}
static unsigned read_in_flight_minimum_snapshot(void){return atomic_load_explicit(&read_in_flight_minimum_count,memory_order_relaxed);}
static void track_read_in_flight_minimum(uint32_t mask){
    if(!atomic_load_explicit(&read_in_flight_minimum_tracking,memory_order_relaxed))return;
    unsigned observed=ns6_midi_in_flight_count(mask);
    unsigned current=atomic_load_explicit(&read_in_flight_minimum_count,memory_order_relaxed);
    while(observed<current&&!atomic_compare_exchange_weak_explicit(&read_in_flight_minimum_count,&current,
        ns6_midi_in_flight_minimum(current,observed),memory_order_relaxed,memory_order_relaxed)){}
}
static void mark_read_slot_out_of_flight(unsigned index){
    if(index>=MIDI_READ_SLOTS)return;
    uint_fast64_t empty=0;
    atomic_compare_exchange_strong_explicit(&read_slot_out_of_flight_since_ms[index],&empty,monotonic_ms(),
        memory_order_release,memory_order_relaxed);
}
static bool wait_for_input_reads_to_drain(void){
    pthread_mutex_lock(&queue_lock);
    while(atomic_load_explicit(&running,memory_order_acquire)&&read_in_flight_snapshot()!=0)
        pthread_cond_wait(&queue_ready,&queue_lock);
    bool drained=read_in_flight_snapshot()==0;
    pthread_mutex_unlock(&queue_lock);
    return drained;
}
static bool recover_stalled_pipe(UInt8 pipe,const char *direction){
    if(!usb||!pipe||!atomic_load(&running))return false;
    pthread_mutex_lock(&pipe_recovery_lock);
    uint64_t started=monotonic_us();
    IOReturn result=(*usb)->ClearPipeStallBothEnds(usb,pipe);
    uint64_t duration=ns6_midi_recovery_elapsed_us(started,monotonic_us());
    pthread_mutex_unlock(&pipe_recovery_lock);
    unsigned endpoint_address=!strcmp(direction,"input")?0x83u:0x04u;
    if(result==kIOReturnSuccess){
        os_log(OS_LOG_DEFAULT,"Numark NS6 MIDI %{public}s pipe stall cleared: endpoint 0x%02x, pipeRef %u, status 0x%08x, duration %llu us",direction,endpoint_address,pipe,(unsigned)result,(unsigned long long)duration);
        return true;
    }
    os_log_error(OS_LOG_DEFAULT,"Numark NS6 MIDI %{public}s pipe stall recovery failed: endpoint 0x%02x, pipeRef %u, status 0x%08x (%{public}u), duration %llu us",direction,endpoint_address,pipe,(unsigned)result,(unsigned)result,(unsigned long long)duration);
    return false;
}
static void schedule_read_recovery(struct read_slot *slot,IOReturn status){
    if(!slot||!atomic_load_explicit(&running,memory_order_acquire))return;
    ptrdiff_t index=slot-reads;
    if(index<0||index>=MIDI_READ_SLOTS)return;
    if(ns6_midi_recovery_withhold_not_responding((uint32_t)status,
            (uint32_t)kIOReturnNotResponding,NS6_MIDI_DIAGNOSTIC_NO_RETRY_NOT_RESPONDING)){
        os_log_error(OS_LOG_DEFAULT,"Numark NS6 MIDI diagnostic: withholding retry after kIOReturnNotResponding on endpoint 0x83 slot %ld; other reads remain active (mask 0x%x)",
            (long)index,read_in_flight_snapshot());
        return;
    }
    bool clear_stall=ns6_midi_recovery_requires_pipe_clear((uint32_t)status,(uint32_t)kIOUSBPipeStalled);
    atomic_store_explicit(&read_recovery_requested_us[index],monotonic_us(),memory_order_release);
    pthread_mutex_lock(&queue_lock);
    pending_read_recovery_mask=ns6_midi_recovery_add(pending_read_recovery_mask,(unsigned)index);
    if(clear_stall)pending_read_clear_mask|=UINT32_C(1)<<(unsigned)index;
    pthread_cond_signal(&queue_ready);
    pthread_mutex_unlock(&queue_lock);
    enum ns6_midi_error_class error_class=ns6_midi_recovery_error_class(
        (uint32_t)status,(uint32_t)kIOReturnNotResponding,(uint32_t)kIOUSBPipeStalled);
    const char *class_name=error_class==NS6_MIDI_ERROR_NOT_RESPONDING?"not-responding":
        error_class==NS6_MIDI_ERROR_PIPE_STALLED?"pipe-stalled":"other";
    os_log(OS_LOG_DEFAULT,"Numark NS6 MIDI input recovery requested: endpoint 0x83, slot %ld, status 0x%08x, class %{public}s, pipe clear %{public}s, in-flight mask 0x%x",
        (long)index,(unsigned)status,class_name,clear_stall?"yes":"no",read_in_flight_snapshot());
}
static void log_input_error(IOReturn result,const char *stage,struct read_slot *slot,uint64_t api_duration_us){
    ptrdiff_t index=slot?slot-reads:-1;
    enum ns6_midi_error_class error_class=ns6_midi_recovery_error_class(
        (uint32_t)result,(uint32_t)kIOReturnNotResponding,(uint32_t)kIOUSBPipeStalled);
    const char *class_name=error_class==NS6_MIDI_ERROR_NOT_RESPONDING?"not-responding":
        error_class==NS6_MIDI_ERROR_PIPE_STALLED?"pipe-stalled":"other";
    if(index<0||index>=MIDI_READ_SLOTS){
        os_log_error(OS_LOG_DEFAULT,"Numark NS6 MIDI input %{public}s failed: invalid read slot %ld, class %{public}s, status 0x%08x (%{public}u)",
            stage,(long)index,class_name,(unsigned)result,(unsigned)result);
        return;
    }
    uint64_t count=atomic_fetch_add_explicit(&input_error_counts[index][error_class],1,memory_order_relaxed)+1;
    uint64_t now=monotonic_ms();
    uint_fast64_t last=atomic_load_explicit(&last_input_error_log_ms,memory_order_relaxed);
    if(!ns6_midi_recovery_log_due(now,last,5000)||
       !atomic_compare_exchange_strong_explicit(&last_input_error_log_ms,&last,now,memory_order_relaxed,memory_order_relaxed))return;
    uint32_t in_flight=read_in_flight_snapshot();
    if(!strcmp(stage,"submission")){
        os_log_error(OS_LOG_DEFAULT,"Numark NS6 MIDI input %{public}s failed: slot %ld, class %{public}s, status 0x%08x (%{public}u), ReadPipeAsync duration %llu us, slot/class count %llu, reads in flight %u/4 (mask 0x%x), minimum %u/4",
            stage,(long)index,class_name,(unsigned)result,(unsigned)result,(unsigned long long)api_duration_us,(unsigned long long)count,
            ns6_midi_in_flight_count(in_flight),in_flight,read_in_flight_minimum_snapshot());
    }else{
        os_log_error(OS_LOG_DEFAULT,"Numark NS6 MIDI input %{public}s failed: slot %ld, class %{public}s, status 0x%08x (%{public}u), slot/class count %llu, reads in flight %u/4 (mask 0x%x), minimum %u/4",
            stage,(long)index,class_name,(unsigned)result,(unsigned)result,(unsigned long long)count,
            ns6_midi_in_flight_count(in_flight),in_flight,read_in_flight_minimum_snapshot());
    }
}

static bool find_pipes(void){
    UInt8 endpoints=0;
    if((*usb)->GetNumEndpoints(usb,&endpoints)!=kIOReturnSuccess)return false;
    for(UInt8 pipe=1;pipe<=endpoints;++pipe){
        UInt8 direction=0,number=0,type=0,interval=0;
        UInt16 maximum=0;
        if((*usb)->GetPipeProperties(usb,pipe,&direction,&number,&type,&maximum,&interval)!=kIOReturnSuccess)continue;
        UInt8 endpoint_address=(direction==kUSBIn?0x80u:0u)|number;
        os_log(OS_LOG_DEFAULT,"Numark NS6 MIDI pipe properties: pipe %u, endpoint 0x%02x, direction %u, type %u, max packet %u, interval %u",
            (unsigned)pipe,(unsigned)endpoint_address,(unsigned)direction,(unsigned)type,(unsigned)maximum,(unsigned)interval);
        if(direction==kUSBIn&&number==3&&type==kUSBBulk)input_pipe=pipe;
        if(direction==kUSBOut&&number==4&&type==kUSBBulk)output_pipe=pipe;
    }
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
static bool submit_read(struct read_slot *slot);
static void read_complete(void *reference,IOReturn status,void *argument){
    struct read_slot *slot=reference;
    uint32_t bit=read_slot_bit(slot);
    uint32_t previous=atomic_fetch_and_explicit(&read_in_flight_mask,~bit,memory_order_acq_rel);
    track_read_in_flight_minimum(read_in_flight_snapshot());
    if(!(previous&bit))os_log_error(OS_LOG_DEFAULT,"Numark NS6 MIDI completion arrived without an in-flight slot (slot %ld, status 0x%08x)",(long)(slot?slot-reads:-1),(unsigned)status);
    if(!ns6_midi_read_completion_should_continue(
           atomic_load_explicit(&running,memory_order_acquire),
           atomic_load_explicit(&read_target_count,memory_order_acquire),
           slot? (unsigned)(slot-reads):MIDI_READ_SLOTS,
           atomic_load_explicit(&input_pipe_recovery_active,memory_order_acquire))){
        pthread_mutex_lock(&queue_lock);
        pthread_cond_broadcast(&queue_ready);
        pthread_mutex_unlock(&queue_lock);
        return;
    }
    if(status==kIOReturnSuccess){UInt32 length=(UInt32)(uintptr_t)argument;if(length>MIDI_PACKET_BYTES)length=MIDI_PACKET_BYTES;publish_packet(slot->data,length);}
    else if(atomic_load(&running)){
        ptrdiff_t index=slot?slot-reads:-1;
        if(index>=0&&index<MIDI_READ_SLOTS)mark_read_slot_out_of_flight((unsigned)index);
        log_input_error(status,"completion",slot,0);
        schedule_read_recovery(slot,status);
        return;
    }
    if(atomic_load(&running))submit_read(slot);
}
static bool submit_read(struct read_slot *slot){
    ptrdiff_t index=slot?slot-reads:-1;
    if(!ns6_midi_read_should_submit(atomic_load_explicit(&running,memory_order_acquire),
            atomic_load_explicit(&read_target_count,memory_order_acquire),
            index>=0?(unsigned)index:MIDI_READ_SLOTS,
            atomic_load_explicit(&input_pipe_recovery_active,memory_order_acquire)))return false;
    uint32_t bit=read_slot_bit(slot);
    uint32_t previous=atomic_fetch_or_explicit(&read_in_flight_mask,bit,memory_order_acq_rel);
    if(previous&bit){
        os_log_error(OS_LOG_DEFAULT,"Numark NS6 MIDI duplicate read submission for slot %ld",(long)(slot-reads));
        return false;
    }
    track_read_in_flight_minimum(read_in_flight_snapshot());
    uint64_t api_started=monotonic_us();
    IOReturn result=(*usb)->ReadPipeAsync(usb,input_pipe,slot->data,sizeof(slot->data),read_complete,slot);
    uint64_t api_duration_us=ns6_midi_recovery_elapsed_us(api_started,monotonic_us());
    if(result==kIOReturnSuccess){
        ptrdiff_t index=slot-reads;
        uint_fast64_t since=index>=0&&index<MIDI_READ_SLOTS?atomic_exchange_explicit(&read_slot_out_of_flight_since_ms[index],0,memory_order_acq_rel):0;
        if(since){uint64_t now=monotonic_ms();os_log(OS_LOG_DEFAULT,"Numark NS6 MIDI read slot %ld restored after %llu ms out of flight; reads in flight %u/4 (mask 0x%x), minimum %u/4",
            (long)index,(unsigned long long)(now-(uint64_t)since),ns6_midi_in_flight_count(read_in_flight_snapshot()),read_in_flight_snapshot(),read_in_flight_minimum_snapshot());}
        return true;
    }
    atomic_fetch_and_explicit(&read_in_flight_mask,~bit,memory_order_acq_rel);
    track_read_in_flight_minimum(read_in_flight_snapshot());
    index=slot-reads;
    if(index>=0&&index<MIDI_READ_SLOTS)mark_read_slot_out_of_flight((unsigned)index);
    log_input_error(result,"submission",slot,api_duration_us);
    schedule_read_recovery(slot,result);
    return false;
}
static void *write_worker(void *unused){
    (void)unused;
    unsigned consecutive_recovery_failures=0;
    unsigned consecutive_clear_failures=0;
    unsigned input_read_depth=NS6_MIDI_INITIAL_READ_COUNT;
    for(;;){
        struct write_packet packet;
        pthread_mutex_lock(&queue_lock);
        while(head==tail&&pending_read_recovery_mask==0&&
              ns6_midi_read_action(atomic_load_explicit(&running,memory_order_acquire),
                  atomic_load_explicit(&read_target_count,memory_order_acquire),input_read_depth)==NS6_MIDI_READ_NOOP&&
              atomic_load(&running))pthread_cond_wait(&queue_ready,&queue_lock);
        if(head==tail&&!atomic_load(&running)){pending_read_recovery_mask=0;pending_read_clear_mask=0;pthread_mutex_unlock(&queue_lock);break;}
        enum NS6MIDIReadAction action=ns6_midi_read_action(
            atomic_load_explicit(&running,memory_order_acquire),
            atomic_load_explicit(&read_target_count,memory_order_acquire),input_read_depth);
        if(action!=NS6_MIDI_READ_NOOP){
            pthread_mutex_unlock(&queue_lock);
            if(action==NS6_MIDI_READ_QUIESCE){
                atomic_store_explicit(&input_pipe_recovery_active,true,memory_order_release);
                IOReturn abort_result=(*usb)->AbortPipe(usb,input_pipe);
                bool drained=abort_result==kIOReturnSuccess&&wait_for_input_reads_to_drain();
                if(drained){
                    pthread_mutex_lock(&queue_lock);
                    pending_read_recovery_mask=0;
                    pending_read_clear_mask=0;
                    pthread_mutex_unlock(&queue_lock);
                    atomic_store_explicit(&input_pipe_recovery_active,false,memory_order_release);
                    input_read_depth=atomic_load_explicit(&read_target_count,memory_order_acquire);
                    unsigned submitted=0;
                    for(unsigned i=0;i<input_read_depth;++i)if(submit_read(&reads[i]))++submitted;
                    if(input_read_depth)
                        os_log(OS_LOG_DEFAULT,"Numark NS6 MIDI input polling reduced while audio is idle: endpoint 0x83, reads %u/%u submitted",submitted,input_read_depth);
                    else
                        os_log(OS_LOG_DEFAULT,"Numark NS6 MIDI input polling suspended while audio is idle: endpoint 0x83 drained");
                }else{
                    os_log_error(OS_LOG_DEFAULT,"Numark NS6 MIDI input suspend failed: endpoint 0x83 AbortPipe 0x%08x, drained %{public}s",(unsigned)abort_result,drained?"yes":"no");
                    unsigned target=atomic_load_explicit(&read_target_count,memory_order_acquire);
                    if(target){
                        atomic_store_explicit(&input_pipe_recovery_active,false,memory_order_release);
                        input_read_depth=target;
                        uint32_t missing=ns6_midi_read_missing_mask(input_read_depth,read_in_flight_snapshot());
                        for(unsigned i=0;i<input_read_depth;++i)
                            if(missing&(UINT32_C(1)<<i))submit_read(&reads[i]);
                    }else{
                        struct timespec retry_delay={.tv_nsec=20000000};nanosleep(&retry_delay,NULL);
                        pthread_mutex_lock(&queue_lock);pthread_cond_signal(&queue_ready);pthread_mutex_unlock(&queue_lock);
                    }
                }
            }else{
                atomic_store_explicit(&input_pipe_recovery_active,false,memory_order_release);
                input_read_depth=atomic_load_explicit(&read_target_count,memory_order_acquire);
                unsigned submitted=0;
                uint32_t missing=ns6_midi_read_missing_mask(input_read_depth,read_in_flight_snapshot());
                for(unsigned i=0;i<input_read_depth;++i)
                    if((missing&(UINT32_C(1)<<i))&&submit_read(&reads[i]))++submitted;
                os_log(OS_LOG_DEFAULT,"Numark NS6 MIDI input polling resumed with audio: endpoint 0x83, added %u reads, %u/4 in flight",submitted,ns6_midi_in_flight_count(read_in_flight_snapshot()));
            }
            continue;
        }
        uint32_t recovery_mask=atomic_load(&running)?ns6_midi_recovery_take(&pending_read_recovery_mask):0;
        uint32_t clear_mask=atomic_load(&running)?ns6_midi_recovery_take(&pending_read_clear_mask):0;
        unsigned queued_output_packets=(tail+MIDI_WRITE_QUEUE-head)%MIDI_WRITE_QUEUE;
        bool clear_pending=(clear_mask&recovery_mask)!=0;
        if(clear_pending)atomic_store_explicit(&input_pipe_recovery_active,true,memory_order_release);
        if(recovery_mask){pthread_mutex_unlock(&queue_lock);
            uint64_t dequeue_us=monotonic_us(),oldest_requested_us=dequeue_us;
            for(unsigned i=0;i<MIDI_READ_SLOTS;++i)if(recovery_mask&(UINT32_C(1)<<i)){
                uint64_t requested=atomic_load_explicit(&read_recovery_requested_us[i],memory_order_acquire);
                if(requested&&requested<oldest_requested_us)oldest_requested_us=requested;
            }
            uint32_t delay_ms=ns6_midi_recovery_retry_backoff_ms(consecutive_recovery_failures,consecutive_clear_failures,clear_pending);
            uint64_t backoff_started_us=monotonic_us();
            if(delay_ms){struct timespec retry_delay={.tv_sec=delay_ms/1000,.tv_nsec=(long)(delay_ms%1000)*1000000L};nanosleep(&retry_delay,NULL);}
            uint64_t backoff_elapsed_us=ns6_midi_recovery_elapsed_us(backoff_started_us,monotonic_us());
            uint64_t dispatch_us=monotonic_us();
            uint32_t in_flight=read_in_flight_snapshot();
            os_log(OS_LOG_DEFAULT,"Numark NS6 MIDI input recovery dispatch: slots 0x%x, clear 0x%x, request-to-worker %llu us, configured backoff %u ms, actual backoff %llu us, request age at dispatch %llu us, reads in flight %u/4 (mask 0x%x), minimum %u/4, queued output packets %u",
                recovery_mask,clear_mask,(unsigned long long)ns6_midi_recovery_elapsed_us(oldest_requested_us,dequeue_us),delay_ms,(unsigned long long)backoff_elapsed_us,
                (unsigned long long)ns6_midi_recovery_elapsed_us(oldest_requested_us,dispatch_us),
                ns6_midi_in_flight_count(in_flight),in_flight,read_in_flight_minimum_snapshot(),queued_output_packets);
            bool recovered=!clear_pending;
            if(clear_pending){
                uint64_t abort_started_us=monotonic_us();
                IOReturn abort_result=(*usb)->AbortPipe(usb,input_pipe);
                uint64_t abort_duration_us=ns6_midi_recovery_elapsed_us(abort_started_us,monotonic_us());
                if(abort_result==kIOReturnSuccess){
                    os_log(OS_LOG_DEFAULT,"Numark NS6 MIDI endpoint-wide recovery: endpoint 0x83 aborted; draining in-flight mask 0x%x before clearing stall",read_in_flight_snapshot());
                    uint64_t drain_started_us=monotonic_us();
                    bool drained=wait_for_input_reads_to_drain();
                    os_log(OS_LOG_DEFAULT,"Numark NS6 MIDI endpoint-wide recovery: endpoint 0x83 drained=%{public}s, mask 0x%x in %llu us",drained?"yes":"no",read_in_flight_snapshot(),(unsigned long long)ns6_midi_recovery_elapsed_us(drain_started_us,monotonic_us()));
                    if(drained&&atomic_load_explicit(&running,memory_order_acquire))
                        recovered=recover_stalled_pipe(input_pipe,"input");
                    else
                        recovered=false;
                    if(drained){
                        pthread_mutex_lock(&queue_lock);
                        pending_read_recovery_mask=0;
                        pending_read_clear_mask=0;
                        pthread_mutex_unlock(&queue_lock);
                        atomic_store_explicit(&input_pipe_recovery_active,false,memory_order_release);
                    }
                }else{
                    os_log_error(OS_LOG_DEFAULT,"Numark NS6 MIDI endpoint-wide recovery: AbortPipe failed for endpoint 0x83, status 0x%08x, duration %llu us; retrying with backoff",(unsigned)abort_result,(unsigned long long)abort_duration_us);
                    recovered=false;
                }
                consecutive_clear_failures=recovered?0:(consecutive_clear_failures<6?consecutive_clear_failures+1:6);
                if(recovered)recovery_mask=ns6_midi_recovery_expand_pipe_stall(recovery_mask)&ns6_midi_read_target_mask(input_read_depth);
                else if(abort_result==kIOReturnSuccess&&read_in_flight_snapshot()==0){
                    recovery_mask=ns6_midi_recovery_expand_pipe_stall(recovery_mask)&ns6_midi_read_target_mask(input_read_depth);
                    clear_mask=recovery_mask;
                }
            }
            bool all_submitted=recovered;
            if(recovered){
                for(unsigned i=0;i<MIDI_READ_SLOTS;++i)
                    if((recovery_mask&(UINT32_C(1)<<i))&&!submit_read(&reads[i]))all_submitted=false;
            }else if(atomic_load(&running)){
                pthread_mutex_lock(&queue_lock);
                pending_read_recovery_mask|=recovery_mask;
                pending_read_clear_mask|=clear_mask&recovery_mask;
                pthread_cond_signal(&queue_ready);
                pthread_mutex_unlock(&queue_lock);
            }
            consecutive_recovery_failures=all_submitted?0:(consecutive_recovery_failures<6?consecutive_recovery_failures+1:6);
            in_flight=read_in_flight_snapshot();
            os_log(OS_LOG_DEFAULT,"Numark NS6 MIDI input recovery resubmission: requested slots 0x%x, all submitted %{public}s, reads in flight %u/4 (mask 0x%x), minimum %u/4",
                recovery_mask,all_submitted?"yes":"no",ns6_midi_in_flight_count(in_flight),in_flight,read_in_flight_minimum_snapshot());
            continue;
        }
        if(head==tail){pthread_mutex_unlock(&queue_lock);continue;}
        packet=queue[head];head=(head+1)%MIDI_WRITE_QUEUE;pthread_mutex_unlock(&queue_lock);
        IOReturn result=(*usb)->WritePipe(usb,output_pipe,packet.data,sizeof(packet.data));
        if(result==kIOUSBPipeStalled&&recover_stalled_pipe(output_pipe,"output"))
            result=(*usb)->WritePipe(usb,output_pipe,packet.data,sizeof(packet.data));
        if(result!=kIOReturnSuccess)os_log_error(OS_LOG_DEFAULT,"Numark NS6 MIDI output failed: %{public}u",(unsigned)result);
    }
    return NULL;
}
static void send_blackout_report(unsigned char *packet,unsigned *reports,unsigned *failures){
    IOReturn result=(*usb)->WritePipe(usb,output_pipe,packet,MIDI_PACKET_BYTES);
    ++*reports;
    if(result!=kIOReturnSuccess){++*failures;os_log_error(OS_LOG_DEFAULT,"Numark NS6 MIDI shutdown blackout write failed: 0x%08x (%u)",(unsigned)result,(unsigned)result);}
}
static void blackout_controller(void){
    if(!usb||!output_pipe)return;
    unsigned char packet[MIDI_PACKET_BYTES];
    const struct timespec settle={.tv_nsec=2000000};
    unsigned reports=0,failures=0;uint64_t started_us=monotonic_us();
    for(unsigned pass=0;pass<2;++pass){
        unsigned used=0;
        memset(packet,MIDI_IDLE_BYTE,sizeof(packet));
        for(unsigned channel=0;channel<=4;++channel){
            for(unsigned cc=0;cc<=0x51;++cc){
                packet[used++]=0xb0+channel;packet[used++]=cc;packet[used++]=0;
                if(used==39){packet[41]=MIDI_TERMINATOR;send_blackout_report(packet,&reports,&failures);nanosleep(&settle,NULL);memset(packet,MIDI_IDLE_BYTE,sizeof(packet));used=0;}
            }
            for(unsigned note=0;note<=0x50;++note){
                packet[used++]=0x80+channel;packet[used++]=note;packet[used++]=0;
                if(used==39){packet[41]=MIDI_TERMINATOR;send_blackout_report(packet,&reports,&failures);nanosleep(&settle,NULL);memset(packet,MIDI_IDLE_BYTE,sizeof(packet));used=0;}
            }
        }
        if(used){packet[41]=MIDI_TERMINATOR;send_blackout_report(packet,&reports,&failures);nanosleep(&settle,NULL);}
    }
    os_log(OS_LOG_DEFAULT,"Numark NS6 MIDI transport-cleanup blackout completed: %u USB reports, %u write failures, %llu us",reports,failures,(unsigned long long)ns6_midi_recovery_elapsed_us(started_us,monotonic_us()));
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
    usb=new_usb;input_pipe=output_pipe=0;head=tail=0;pending_read_recovery_mask=pending_read_clear_mask=0;memset(&parser,0,sizeof(parser));writer_started=receiver_started=false;
    atomic_store(&read_in_flight_mask,0);atomic_store(&read_in_flight_minimum_count,MIDI_READ_SLOTS);atomic_store(&read_in_flight_minimum_tracking,false);
    atomic_store(&input_pipe_recovery_active,false);
    atomic_store(&audio_client_active,NS6_MIDI_INITIAL_READ_COUNT>=NS6_MIDI_ACTIVE_READ_COUNT);
    atomic_store(&read_target_count,ns6_midi_read_initial_count(true));
    atomic_store(&last_input_error_log_ms,0);
    for(unsigned i=0;i<MIDI_READ_SLOTS;++i)atomic_store(&read_slot_out_of_flight_since_ms[i],0);
    if(!find_pipes()||(ns6_usb_midi_input_enabled()&&(*usb)->ClearPipeStallBothEnds(usb,input_pipe)!=kIOReturnSuccess)||(*usb)->ClearPipeStallBothEnds(usb,output_pipe)!=kIOReturnSuccess||!open_sockets()){close_sockets();usb=NULL;return false;}
    atomic_store(&running,true);
    if(pthread_create(&writer,NULL,write_worker,NULL)!=0){atomic_store(&running,false);close_sockets();usb=NULL;return false;}
    writer_started=true;
    if(ns6_usb_midi_input_enabled()){
        if(pthread_create(&receiver,NULL,receive_worker,NULL)!=0){ns6_midi_stop();return false;}
        receiver_started=true;
        unsigned initial_reads=ns6_midi_read_initial_count(true);
        for(unsigned i=0;i<initial_reads;++i)submit_read(&reads[i]);
        uint32_t initial_in_flight=read_in_flight_snapshot();
        atomic_store(&read_in_flight_minimum_count,ns6_midi_in_flight_count(initial_in_flight));
        atomic_store_explicit(&read_in_flight_minimum_tracking,true,memory_order_release);
        track_read_in_flight_minimum(read_in_flight_snapshot());
        os_log(OS_LOG_DEFAULT,"Numark NS6 MIDI input polling started: reads in flight %u/4 (mask 0x%x), target %u/4, minimum %u/4",
            ns6_midi_in_flight_count(read_in_flight_snapshot()),read_in_flight_snapshot(),initial_reads,read_in_flight_minimum_snapshot());
    }else{
        os_log(OS_LOG_DEFAULT,"Numark NS6 diagnostic mode: MIDI input polling disabled; outbound activation sequence remains enabled");
    }
    return true;
}
void ns6_midi_set_audio_active(bool active){
    if(!ns6_usb_midi_input_enabled())return;
    atomic_store_explicit(&audio_client_active,active,memory_order_release);
    unsigned target=ns6_midi_read_target_count(atomic_load_explicit(&running,memory_order_acquire),active);
    atomic_store_explicit(&read_target_count,target,memory_order_release);
    if(!active&&NS6_MIDI_KEEP_INPUT_IDLE)
        os_log(OS_LOG_DEFAULT,"Numark NS6 diagnostic: keeping MIDI input reads active while CoreAudio is idle");
    pthread_mutex_lock(&queue_lock);
    pthread_cond_signal(&queue_ready);
    pthread_mutex_unlock(&queue_lock);
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
    atomic_store_explicit(&audio_client_active,false,memory_order_release);
    bool was_running=atomic_exchange(&running,false);
    atomic_store_explicit(&read_target_count,0,memory_order_release);
    if(!was_running&&!writer_started&&!receiver_started){close_sockets();usb=NULL;return;}
    pthread_mutex_lock(&queue_lock);pthread_cond_broadcast(&queue_ready);pthread_mutex_unlock(&queue_lock);
    if(usb&&input_pipe)(*usb)->AbortPipe(usb,input_pipe);
    if(inbound_socket>=0){close(inbound_socket);inbound_socket=-1;}
    if(writer_started){pthread_join(writer,NULL);writer_started=false;}
    /* The HAL still owns the USB interface here. Do the blackout before the
       audio shutdown releases it; Mixxx may already have destroyed its MIDI
       endpoint and cannot be relied on to call the mapping shutdown hook. */
    if(blackout){os_log(OS_LOG_DEFAULT,"Numark NS6 MIDI transport cleanup requested controller blackout");blackout_controller();}
    else os_log(OS_LOG_DEFAULT,"Numark NS6 MIDI transport restart skipped controller blackout");
    if(receiver_started){pthread_join(receiver,NULL);receiver_started=false;}
    if(outbound_socket>=0){close(outbound_socket);outbound_socket=-1;}
    usb=NULL;input_pipe=output_pipe=0;
}
void ns6_midi_stop(void){stop_midi(true);}
void ns6_midi_stop_for_audio_restart(void){stop_midi(false);}
