#include "NS6USB.h"
#include "NS6Transport.h"
#include "NS6MIDI.h"
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/usb/IOUSBLib.h>
#include <pthread.h>
#include <stdatomic.h>
#include <os/log.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define VID 0x15e4
#define PID 0x0079
#define RATE 44100
#define FRAME_BYTES 12
#define PACKETS 32
#define FEEDBACK_PACKETS 32
#define FEEDBACK_MAX_PACKET 64
#define SLOT_COUNT 8
#define MAX_PACKET 156

struct slot { IOUSBLowLatencyIsocFrame *frames; unsigned char *data; unsigned consecutive_errors; };
struct feedback_slot { IOUSBLowLatencyIsocFrame *frames; unsigned char *data; };
static IOUSBInterfaceInterface **interface;
static IOUSBInterfaceInterface **auxiliary_interface;
static CFRunLoopSourceRef source;
static CFRunLoopSourceRef feedback_source;
static CFRunLoopRef run_loop;
static struct slot slots[SLOT_COUNT];
static struct feedback_slot feedback;
static UInt8 feedback_pipe;
static UInt16 feedback_packet_bytes;
static pthread_t thread;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static atomic_bool running;
static atomic_bool recovery_requested;
static atomic_bool buffer_restart_requested;
static atomic_bool output_paused=true;
static atomic_bool awaiting_audio_prefill;
static atomic_uint startup_buffer_frames=256;
static atomic_bool standby_prime_pending;
static atomic_uint_fast64_t completed_transfers;
static atomic_uint_fast64_t underrun_frames;
static atomic_uint_fast64_t underrun_events;
static atomic_uint_fast64_t underrun_max_burst;
static atomic_uint_fast64_t isoc_packet_errors;
static atomic_uint_fast64_t isoc_short_packets;
static atomic_uint_fast64_t isoc_short_bytes;
static atomic_uint_fast64_t isoc_transfer_errors;
static atomic_uint_fast64_t last_underrun_log_ms;
static atomic_uint_fast64_t last_packet_log_ms;
static atomic_int last_packet_status;
static atomic_uint_fast64_t feedback_samples;
static atomic_uint_fast64_t feedback_44_frames;
static atomic_uint_fast64_t feedback_45_frames;
static atomic_uint_fast64_t feedback_other_values;
static atomic_uint_fast64_t feedback_packet_errors;
static atomic_uint_fast64_t feedback_last_log_ms;
static unsigned char feedback_last_sample[3];
static uint64_t feedback_logged_samples, feedback_logged_44, feedback_logged_45, feedback_logged_other;
static bool thread_created, initialized;
static uint64_t fraction;

static uint64_t monotonic_ms(void){struct timespec now;clock_gettime(CLOCK_MONOTONIC,&now);return (uint64_t)now.tv_sec*1000+(uint64_t)now.tv_nsec/1000000;}
static void update_max(atomic_uint_fast64_t *value,uint64_t candidate){
    uint_fast64_t previous=atomic_load_explicit(value,memory_order_relaxed);
    while(candidate>previous&&!atomic_compare_exchange_weak_explicit(value,&previous,candidate,memory_order_relaxed,memory_order_relaxed)){}
}
static bool should_log_once_per_second(atomic_uint_fast64_t *last_log){
    uint_fast64_t now=monotonic_ms(),previous=atomic_load_explicit(last_log,memory_order_relaxed);
    return now-previous>=1000&&atomic_compare_exchange_strong_explicit(last_log,&previous,now,memory_order_relaxed,memory_order_relaxed);
}

static int property_u16(io_registry_entry_t entry, CFStringRef key, UInt16 *out) {
    CFTypeRef value=IORegistryEntryCreateCFProperty(entry,key,kCFAllocatorDefault,0);
    int ok=value&&CFGetTypeID(value)==CFNumberGetTypeID()&&CFNumberGetValue(value,kCFNumberSInt16Type,out);
    if(value)CFRelease(value); return ok;
}
static bool is_ns6_interface(io_registry_entry_t service){
    io_registry_entry_t entry=service;
    for(;;){
        UInt16 vendor=0,product=0;
        if(property_u16(entry,CFSTR(kUSBVendorID),&vendor)&&property_u16(entry,CFSTR(kUSBProductID),&product)&&vendor==VID&&product==PID){if(entry!=service)IOObjectRelease(entry);return true;}
        io_registry_entry_t parent=IO_OBJECT_NULL;
        if(IORegistryEntryGetParentEntry(entry,kIOServicePlane,&parent)!=KERN_SUCCESS){if(entry!=service)IOObjectRelease(entry);return false;}
        if(entry!=service)IOObjectRelease(entry); entry=parent;
    }
}
static IOUSBInterfaceInterface **open_interface_once(UInt8 wanted_number) {
    io_iterator_t iterator=IO_OBJECT_NULL; io_service_t service;
    if(IOServiceGetMatchingServices(kIOMainPortDefault,IOServiceMatching("IOUSBHostInterface"),&iterator)!=KERN_SUCCESS)return NULL;
    while((service=IOIteratorNext(iterator))){
        UInt8 number=255;
        if(!is_ns6_interface(service)){IOObjectRelease(service);continue;}
        IOCFPlugInInterface **plugin=NULL; IOUSBInterfaceInterface **candidate=NULL; SInt32 score=0;
        HRESULT result=IOCreatePlugInInterfaceForService(service,kIOUSBInterfaceUserClientTypeID,kIOCFPlugInInterfaceID,&plugin,&score);
        if(result==kIOReturnSuccess&&plugin){result=(*plugin)->QueryInterface(plugin,CFUUIDGetUUIDBytes(kIOUSBInterfaceInterfaceID),(LPVOID)&candidate);IODestroyPlugInInterface(plugin);} IOObjectRelease(service);
        if(result||!candidate)continue; (*candidate)->GetInterfaceNumber(candidate,&number);
        if(number==wanted_number){IOObjectRelease(iterator);return candidate;} (*candidate)->Release(candidate);
    }
    IOObjectRelease(iterator); return NULL;
}
static bool configure_ns6_device_if_needed(void){
    io_iterator_t iterator=IO_OBJECT_NULL; io_service_t service;
    if(IOServiceGetMatchingServices(kIOMainPortDefault,IOServiceMatching("IOUSBHostDevice"),&iterator)!=KERN_SUCCESS)return false;
    while((service=IOIteratorNext(iterator))){
        UInt16 vendor=0,product=0;
        if(!property_u16(service,CFSTR(kUSBVendorID),&vendor)||!property_u16(service,CFSTR(kUSBProductID),&product)||vendor!=VID||product!=PID){IOObjectRelease(service);continue;}
        IOCFPlugInInterface **plugin=NULL; IOUSBDeviceInterface **device=NULL; SInt32 score=0;
        HRESULT result=IOCreatePlugInInterfaceForService(service,kIOUSBDeviceUserClientTypeID,kIOCFPlugInInterfaceID,&plugin,&score);
        if(result==kIOReturnSuccess&&plugin){result=(*plugin)->QueryInterface(plugin,CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID),(LPVOID)&device);IODestroyPlugInInterface(plugin);} IOObjectRelease(service); IOObjectRelease(iterator);
        if(result||!device)return false;
        IOReturn opened=(*device)->USBDeviceOpenSeize(device);
        IOReturn configured=opened;
        if(opened==kIOReturnSuccess){
            UInt8 current_configuration=0;
            IOReturn queried=(*device)->GetConfiguration(device,&current_configuration);
            if(queried==kIOReturnSuccess){
                configured=(*device)->SetConfiguration(device,1);
                if(configured==kIOReturnSuccess)os_log_info(OS_LOG_DEFAULT,"Numark NS6 USB interface was absent; refreshed configuration 1 (previously %u)",current_configuration);
            }else configured=queried;
            (*device)->USBDeviceClose(device);
        }
        (*device)->Release(device); return configured==kIOReturnSuccess;
    }
    IOObjectRelease(iterator); return false;
}
static IOUSBInterfaceInterface **open_interface(void){
    const struct timespec delay={0,100000000};
    for(unsigned attempt=0;attempt<10;++attempt){
        IOUSBInterfaceInterface **candidate=open_interface_once(0); if(candidate)return candidate;
        nanosleep(&delay,NULL);
    }
    if(!configure_ns6_device_if_needed())return NULL;
    for(unsigned attempt=0;attempt<20;++attempt){
        IOUSBInterfaceInterface **candidate=open_interface_once(0); if(candidate)return candidate;
        nanosleep(&delay,NULL);
    }
    os_log_error(OS_LOG_DEFAULT,"Numark NS6 USB interface 0 did not appear after conditional configuration recovery");
    return NULL;
}
static void prepare_feedback_reader(void){
    IOUSBInterfaceInterface **feedback_interface=auxiliary_interface;
    UInt8 endpoints=0;
    if(!feedback_interface||(*feedback_interface)->GetNumEndpoints(feedback_interface,&endpoints)!=kIOReturnSuccess)return;
    for(UInt8 pipe=1;pipe<=endpoints;++pipe){
        UInt8 direction=0,number=0,type=0,interval=0;UInt16 maximum=0;
        if((*feedback_interface)->GetPipeProperties(feedback_interface,pipe,&direction,&number,&type,&maximum,&interval)!=kIOReturnSuccess)continue;
        if(direction!=kUSBIn||number!=1||type!=kUSBIsoc)continue;
        feedback_pipe=pipe;feedback_packet_bytes=maximum&0x07ff;
        if(feedback_packet_bytes>FEEDBACK_MAX_PACKET)feedback_packet_bytes=FEEDBACK_MAX_PACKET;
        if(feedback_packet_bytes<3){feedback_pipe=0;feedback_packet_bytes=0;return;}
        IOReturn result=(*feedback_interface)->LowLatencyCreateBuffer(feedback_interface,(void**)&feedback.data,FEEDBACK_PACKETS*feedback_packet_bytes,kUSBLowLatencyReadBuffer);
        if(result==kIOReturnSuccess)result=(*feedback_interface)->LowLatencyCreateBuffer(feedback_interface,(void**)&feedback.frames,sizeof(*feedback.frames)*FEEDBACK_PACKETS,kUSBLowLatencyFrameListBuffer);
        if(result!=kIOReturnSuccess){
            if(feedback.data)(*feedback_interface)->LowLatencyDestroyBuffer(feedback_interface,feedback.data);
            if(feedback.frames)(*feedback_interface)->LowLatencyDestroyBuffer(feedback_interface,feedback.frames);
            memset(&feedback,0,sizeof(feedback));feedback_pipe=0;feedback_packet_bytes=0;
            os_log_error(OS_LOG_DEFAULT,"Numark NS6 optional clock feedback reader allocation failed: 0x%08x",(unsigned)result);
            return;
        }
        os_log(OS_LOG_DEFAULT,"Numark NS6 optional clock feedback reader found on interface 1 pipe %u (packet capacity %u, interval %u)",pipe,feedback_packet_bytes,interval);
        return;
    }
    os_log(OS_LOG_DEFAULT,"Numark NS6 optional clock feedback endpoint 0x81 is unavailable; audio output remains enabled");
}
static void feedback_submit(void);
static void feedback_complete(void *reference,IOReturn status,void *argument){
    (void)argument;
    struct feedback_slot *slot=reference;
    if(!atomic_load_explicit(&running,memory_order_acquire))return;
    if(status!=kIOReturnSuccess){
        uint64_t errors=atomic_fetch_add_explicit(&feedback_packet_errors,1,memory_order_relaxed)+1;
        if(should_log_once_per_second(&feedback_last_log_ms))
            os_log_error(OS_LOG_DEFAULT,"Numark NS6 feedback endpoint read failed: 0x%08x (%llu errors)",(unsigned)status,(unsigned long long)errors);
        feedback_submit();return;
    }
    for(unsigned p=0;p<FEEDBACK_PACKETS;++p){
        IOUSBLowLatencyIsocFrame *frame=&slot->frames[p];
        if(frame->frStatus!=kIOReturnSuccess){atomic_fetch_add_explicit(&feedback_packet_errors,1,memory_order_relaxed);continue;}
        if(frame->frActCount<3)continue;
        const unsigned char *packet=slot->data+p*feedback_packet_bytes;
        feedback_last_sample[0]=packet[0];feedback_last_sample[1]=packet[1];feedback_last_sample[2]=packet[2];
        atomic_fetch_add_explicit(&feedback_samples,1,memory_order_relaxed);
        if(packet[0]==44)atomic_fetch_add_explicit(&feedback_44_frames,1,memory_order_relaxed);
        else if(packet[0]==45)atomic_fetch_add_explicit(&feedback_45_frames,1,memory_order_relaxed);
        else atomic_fetch_add_explicit(&feedback_other_values,1,memory_order_relaxed);
    }
    if(should_log_once_per_second(&feedback_last_log_ms)){
        uint64_t samples=atomic_load_explicit(&feedback_samples,memory_order_relaxed);
        uint64_t count44=atomic_load_explicit(&feedback_44_frames,memory_order_relaxed);
        uint64_t count45=atomic_load_explicit(&feedback_45_frames,memory_order_relaxed);
        uint64_t other=atomic_load_explicit(&feedback_other_values,memory_order_relaxed);
        uint64_t window44=count44-feedback_logged_44, window45=count45-feedback_logged_45;
        uint64_t window_other=other-feedback_logged_other, window_samples=samples-feedback_logged_samples;
        uint64_t valid=window44+window45;
        double average=valid?44.0+(double)window45/(double)valid:0.0;
        os_log(OS_LOG_DEFAULT,"Numark NS6 USB clock feedback window: %llu samples, 44=%llu, 45=%llu, other=%llu, average %.4f frames/valid report; cumulative %llu samples; last %02x %02x %02x; read errors %llu",(unsigned long long)window_samples,(unsigned long long)window44,(unsigned long long)window45,(unsigned long long)window_other,average,(unsigned long long)samples,feedback_last_sample[0],feedback_last_sample[1],feedback_last_sample[2],(unsigned long long)atomic_load_explicit(&feedback_packet_errors,memory_order_relaxed));
        feedback_logged_samples=samples;feedback_logged_44=count44;feedback_logged_45=count45;feedback_logged_other=other;
    }
    feedback_submit();
}
static void feedback_submit(void){
    if(!feedback_pipe||!feedback.data||!feedback.frames||!atomic_load_explicit(&running,memory_order_acquire))return;
    for(unsigned p=0;p<FEEDBACK_PACKETS;++p){feedback.frames[p].frReqCount=feedback_packet_bytes;feedback.frames[p].frActCount=0;feedback.frames[p].frStatus=0;}
    IOReturn result=(*auxiliary_interface)->LowLatencyReadIsochPipeAsync(auxiliary_interface,feedback_pipe,feedback.data,0,FEEDBACK_PACKETS,1,feedback.frames,feedback_complete,&feedback);
    if(result!=kIOReturnSuccess){
        uint64_t errors=atomic_fetch_add_explicit(&feedback_packet_errors,1,memory_order_relaxed)+1;
        if(should_log_once_per_second(&feedback_last_log_ms))
            os_log_error(OS_LOG_DEFAULT,"Numark NS6 feedback endpoint submit failed: 0x%08x (%llu errors)",(unsigned)result,(unsigned long long)errors);
    }
}
static bool usb_failure(const char *operation,IOReturn result){fprintf(stderr,"Numark NS6 %s failed: 0x%08x\n",operation,result);os_log_error(OS_LOG_DEFAULT,"Numark NS6 %{public}s failed: 0x%08x",operation,(unsigned)result);return false;}
static IOReturn control(UInt8 type,UInt8 request,UInt16 value,UInt16 index,void *data,UInt16 length){
    IOUSBDevRequest r={type,request,value,index,length,data,0}; return (*interface)->ControlRequest(interface,0,&r);
}
static void fill(struct slot *slot){
    unsigned char *output=slot->data;
    for(unsigned packet=0;packet<PACKETS;++packet){
        /* Whole 12-byte frames, averaging exactly 44,100 frames/s. */
        fraction+=RATE;
        unsigned frames=(unsigned)(fraction/8000); fraction%=8000;
        slot->frames[packet].frReqCount=(UInt16)(frames*FRAME_BYTES); slot->frames[packet].frActCount=0; slot->frames[packet].frStatus=0;
        if(atomic_load_explicit(&output_paused,memory_order_acquire)){
            ns6_transport_discard();
            memset(output,0,frames*FRAME_BYTES);
            /* Some NS6 units keep the USB indicator in its searching state
               until the first non-zero audio sample arrives. Prime one USB
               service interval at the smallest 24-bit PCM value; this is
               far below the DAC's analog noise floor and is not an audible
               tone. */
            if(frames&&atomic_exchange_explicit(&standby_prime_pending,false,memory_order_acq_rel))
                for(unsigned frame=0;frame<frames;++frame)
                    for(unsigned channel=0;channel<4;++channel)
                        output[frame*FRAME_BYTES+channel*3]=1;
            output+=frames*FRAME_BYTES;
            continue;
        }
        if(atomic_load_explicit(&awaiting_audio_prefill,memory_order_acquire)){
            if(ns6_transport_available()<atomic_load_explicit(&startup_buffer_frames,memory_order_acquire)){
                memset(output,0,frames*FRAME_BYTES);
                output+=frames*FRAME_BYTES;
                continue;
            }
            atomic_store_explicit(&awaiting_audio_prefill,false,memory_order_release);
            os_log(OS_LOG_DEFAULT,"Numark NS6 startup prebuffer reached %u frames",atomic_load_explicit(&startup_buffer_frames,memory_order_relaxed));
        }
        UInt32 received=ns6_transport_dequeue(output,frames);
        if(received<frames){
            uint64_t missing=frames-received;
            uint64_t previous=atomic_fetch_add_explicit(&underrun_frames,missing,memory_order_relaxed);
            atomic_fetch_add_explicit(&underrun_events,1,memory_order_relaxed);
            update_max(&underrun_max_burst,missing);
            if(previous/4410!=(previous+missing)/4410)
                os_log_error(OS_LOG_DEFAULT,"Numark NS6 audio queue underrun: %llu silent frames total",(unsigned long long)(previous+missing));
            if(should_log_once_per_second(&last_underrun_log_ms))
                os_log_error(OS_LOG_DEFAULT,"Numark NS6 underrun detail: missing %llu frames in this USB packet; %llu events, %llu frames total, max burst %llu, queue now %u frames",(unsigned long long)missing,(unsigned long long)atomic_load_explicit(&underrun_events,memory_order_relaxed),(unsigned long long)(previous+missing),(unsigned long long)atomic_load_explicit(&underrun_max_burst,memory_order_relaxed),ns6_transport_available());
            memset(output+received*FRAME_BYTES,0,(frames-received)*FRAME_BYTES);
        }
        output+=frames*FRAME_BYTES;
    }
}
static void submit(struct slot *slot);
static void request_recovery(const char *operation,IOReturn result){
    fprintf(stderr,"Numark NS6 %s failed: 0x%08x; restarting USB audio stream\n",operation,result);
    os_log_error(OS_LOG_DEFAULT,"Numark NS6 %{public}s failed: 0x%08x; restarting USB audio stream",operation,(unsigned)result);
    atomic_store_explicit(&recovery_requested,true,memory_order_release);
    if(run_loop)CFRunLoopWakeUp(run_loop);
}
static void complete(void *reference,IOReturn status,void *argument){
    (void)argument;
    struct slot *slot=reference;
    if(!atomic_load_explicit(&running,memory_order_acquire)||atomic_load_explicit(&recovery_requested,memory_order_acquire))return;
    if(status!=kIOReturnSuccess){
        atomic_fetch_add_explicit(&isoc_transfer_errors,1,memory_order_relaxed);
        ++slot->consecutive_errors;
        fprintf(stderr,"Numark NS6 isochronous completion failed: 0x%08x (%u consecutive)\n",status,slot->consecutive_errors);
        os_log_error(OS_LOG_DEFAULT,"Numark NS6 isochronous completion failed: 0x%08x (%u consecutive)",(unsigned)status,slot->consecutive_errors);
        if(slot->consecutive_errors>=3)request_recovery("isochronous transfer",status);
        else submit(slot);
        return;
    }
    slot->consecutive_errors=0;
    UInt64 short_bytes=0;
    UInt32 short_packets=0;
    UInt32 packet_errors=0;
    IOReturn packet_error_status=kIOReturnSuccess;
    for(unsigned p=0;p<PACKETS;++p){
        IOUSBLowLatencyIsocFrame *frame=&slot->frames[p];
        if(frame->frStatus!=kIOReturnSuccess){
            ++packet_errors;
            packet_error_status=frame->frStatus;
        }else if(frame->frActCount<frame->frReqCount){
            ++short_packets;
            short_bytes+=(UInt64)(frame->frReqCount-frame->frActCount);
        }
    }
    if(packet_errors){
        atomic_fetch_add_explicit(&isoc_packet_errors,packet_errors,memory_order_relaxed);
        atomic_store_explicit(&last_packet_status,packet_error_status,memory_order_relaxed);
        if(should_log_once_per_second(&last_packet_log_ms))
            os_log_error(OS_LOG_DEFAULT,"Numark NS6 isochronous packet errors: %u in last transfer, status 0x%08x; %llu cumulative packet errors",packet_errors,(unsigned)packet_error_status,(unsigned long long)atomic_load_explicit(&isoc_packet_errors,memory_order_relaxed));
    }
    if(short_packets){
        atomic_fetch_add_explicit(&isoc_short_packets,short_packets,memory_order_relaxed);
        atomic_fetch_add_explicit(&isoc_short_bytes,short_bytes,memory_order_relaxed);
        if(should_log_once_per_second(&last_packet_log_ms))
            os_log_error(OS_LOG_DEFAULT,"Numark NS6 short isochronous output: %u packets, %llu missing bytes in last transfer; %llu short packets, %llu bytes cumulative",short_packets,(unsigned long long)short_bytes,(unsigned long long)atomic_load_explicit(&isoc_short_packets,memory_order_relaxed),(unsigned long long)atomic_load_explicit(&isoc_short_bytes,memory_order_relaxed));
    }
    uint64_t completed=atomic_fetch_add_explicit(&completed_transfers,1,memory_order_relaxed)+1;
    if(completed%10000==0)
        os_log(OS_LOG_DEFAULT,"Numark NS6 audio health: transfers %llu, underruns %llu events/%llu frames (max burst %llu), USB transfer errors %llu, packet errors %llu (last 0x%08x), short packets %llu/%llu bytes, queue %u frames",(unsigned long long)completed,(unsigned long long)atomic_load_explicit(&underrun_events,memory_order_relaxed),(unsigned long long)atomic_load_explicit(&underrun_frames,memory_order_relaxed),(unsigned long long)atomic_load_explicit(&underrun_max_burst,memory_order_relaxed),(unsigned long long)atomic_load_explicit(&isoc_transfer_errors,memory_order_relaxed),(unsigned long long)atomic_load_explicit(&isoc_packet_errors,memory_order_relaxed),(unsigned)atomic_load_explicit(&last_packet_status,memory_order_relaxed),(unsigned long long)atomic_load_explicit(&isoc_short_packets,memory_order_relaxed),(unsigned long long)atomic_load_explicit(&isoc_short_bytes,memory_order_relaxed),ns6_transport_available());
    submit(slot);
}
static void submit(struct slot *slot){
    if(!atomic_load_explicit(&running,memory_order_acquire)||atomic_load_explicit(&recovery_requested,memory_order_acquire))return;
    fill(slot); IOReturn result=(*interface)->LowLatencyWriteIsochPipeAsync(interface,1,slot->data,0,PACKETS,1,slot->frames,complete,slot);
    if(result!=kIOReturnSuccess)request_recovery("submit isochronous transfer",result);
}
static bool initialize(void){
    unsigned char capability[64]={0},rate[3]={0x44,0xac,0}; interface=open_interface(); if(!interface){os_log_error(OS_LOG_DEFAULT,"Numark NS6 could not find USB interface 0");return false;}
    IOReturn result=(*interface)->USBInterfaceOpenSeize(interface); if(result!=kIOReturnSuccess)return usb_failure("open interface",result);
    auxiliary_interface=open_interface_once(1); if(!auxiliary_interface){os_log_error(OS_LOG_DEFAULT,"Numark NS6 could not find USB interface 1");return false;}
    result=(*auxiliary_interface)->USBInterfaceOpenSeize(auxiliary_interface); if(result!=kIOReturnSuccess)return usb_failure("open auxiliary interface",result);
    result=(*auxiliary_interface)->SetAlternateInterface(auxiliary_interface,1); if(result!=kIOReturnSuccess)return usb_failure("select auxiliary alternate setting",result);
    result=(*interface)->CreateInterfaceAsyncEventSource(interface,&source); if(result!=kIOReturnSuccess||!source)return usb_failure("create async source",result);
    run_loop=CFRunLoopGetCurrent(); CFRetain(run_loop); CFRunLoopAddSource(run_loop,source,kCFRunLoopDefaultMode);
    result=(*auxiliary_interface)->CreateInterfaceAsyncEventSource(auxiliary_interface,&feedback_source); if(result!=kIOReturnSuccess||!feedback_source)return usb_failure("create feedback async source",result);
    CFRunLoopAddSource(run_loop,feedback_source,kCFRunLoopDefaultMode);
    result=(*interface)->SetAlternateInterface(interface,1); if(result!=kIOReturnSuccess)return usb_failure("select alternate setting",result);
    result=control(0xc0,86,0,0,capability,8); if(result!=kIOReturnSuccess)return usb_failure("read capability",result);
    UInt16 capability_length=capability[0]<sizeof(capability)?capability[0]:(UInt16)sizeof(capability);
    if(capability_length){result=control(0xc0,86,0,0,capability,capability_length);if(result!=kIOReturnSuccess)return usb_failure("read capability details",result);}
    result=control(0x22,1,0x0100,134,rate,sizeof(rate));if(result!=kIOReturnSuccess)return usb_failure("set clock selector 134",result);
    result=control(0x22,1,0x0100,2,rate,sizeof(rate));if(result!=kIOReturnSuccess)return usb_failure("set clock selector 2",result);
    result=control(0x40,73,0x0032,0,NULL,0);if(result!=kIOReturnSuccess)return usb_failure("start audio engine",result);
    prepare_feedback_reader();
    for(unsigned i=0;i<SLOT_COUNT;++i){
        result=(*interface)->LowLatencyCreateBuffer(interface,(void**)&slots[i].data,PACKETS*MAX_PACKET,kUSBLowLatencyWriteBuffer);if(result!=kIOReturnSuccess)return usb_failure("create audio buffer",result);
        result=(*interface)->LowLatencyCreateBuffer(interface,(void**)&slots[i].frames,sizeof(*slots[i].frames)*PACKETS,kUSBLowLatencyFrameListBuffer);if(result!=kIOReturnSuccess)return usb_failure("create frame list",result);
    }
    return true;
}
static void cleanup(void){
    if(atomic_exchange_explicit(&buffer_restart_requested,false,memory_order_acq_rel))ns6_midi_stop_for_audio_restart();else ns6_midi_stop();
    if(interface)(*interface)->AbortPipe(interface,1);if(auxiliary_interface&&feedback_pipe)(*auxiliary_interface)->AbortPipe(auxiliary_interface,feedback_pipe); if(run_loop)CFRunLoopRunInMode(kCFRunLoopDefaultMode,0.2,false);
    if(auxiliary_interface){if(feedback.data)(*auxiliary_interface)->LowLatencyDestroyBuffer(auxiliary_interface,feedback.data);if(feedback.frames)(*auxiliary_interface)->LowLatencyDestroyBuffer(auxiliary_interface,feedback.frames);}memset(&feedback,0,sizeof(feedback));feedback_pipe=0;feedback_packet_bytes=0;
    if(interface)for(unsigned i=0;i<SLOT_COUNT;++i){if(slots[i].data)(*interface)->LowLatencyDestroyBuffer(interface,slots[i].data);if(slots[i].frames)(*interface)->LowLatencyDestroyBuffer(interface,slots[i].frames);} memset(slots,0,sizeof(slots));
    if(run_loop&&source)CFRunLoopRemoveSource(run_loop,source,kCFRunLoopDefaultMode); if(source)CFRelease(source); source=NULL;
    if(run_loop&&feedback_source)CFRunLoopRemoveSource(run_loop,feedback_source,kCFRunLoopDefaultMode); if(feedback_source)CFRelease(feedback_source); feedback_source=NULL;
    if(run_loop)CFRelease(run_loop); run_loop=NULL;
    if(auxiliary_interface){(*auxiliary_interface)->USBInterfaceClose(auxiliary_interface);(*auxiliary_interface)->Release(auxiliary_interface);} auxiliary_interface=NULL;
    if(interface){(*interface)->USBInterfaceClose(interface);(*interface)->Release(interface);} interface=NULL;
}
static void *worker(void *unused){
    (void)unused;
    bool first_attempt=true;
    const struct timespec recovery_delay={0,500000000};
    while(atomic_load_explicit(&running,memory_order_acquire)){
        atomic_store_explicit(&recovery_requested,false,memory_order_release);
        bool ready=initialize();
        if(first_attempt){
            pthread_mutex_lock(&lock); initialized=ready; if(!ready)atomic_store(&running,false); pthread_cond_broadcast(&changed); pthread_mutex_unlock(&lock);
            first_attempt=false;
            if(!ready){os_log_error(OS_LOG_DEFAULT,"Numark NS6 initial USB audio setup failed");cleanup();break;}
        }else if(!ready){
            pthread_mutex_lock(&lock); initialized=false; pthread_cond_broadcast(&changed); pthread_mutex_unlock(&lock);
            os_log_error(OS_LOG_DEFAULT,"Numark NS6 USB audio recovery setup failed; retrying in 500 ms");
            cleanup();
            if(atomic_load_explicit(&running,memory_order_acquire))nanosleep(&recovery_delay,NULL);
            continue;
        }else{
            pthread_mutex_lock(&lock); initialized=true; pthread_cond_broadcast(&changed); pthread_mutex_unlock(&lock);
        }
        for(unsigned i=0;i<SLOT_COUNT;++i)submit(&slots[i]);
        feedback_submit();
        if(ns6_midi_start(interface,run_loop))ns6_midi_handshake(run_loop);else fprintf(stderr,"Numark NS6 MIDI unavailable; continuing with audio only\n");
        while(atomic_load_explicit(&running,memory_order_acquire)&&!atomic_load_explicit(&recovery_requested,memory_order_acquire))CFRunLoopRunInMode(kCFRunLoopDefaultMode,0.1,false);
        pthread_mutex_lock(&lock); initialized=false; pthread_cond_broadcast(&changed); pthread_mutex_unlock(&lock);
        cleanup();
        if(atomic_load_explicit(&running,memory_order_acquire)){
            fprintf(stderr,"Numark NS6 attempting automatic USB audio recovery\n");
            os_log(OS_LOG_DEFAULT,"Numark NS6 attempting automatic USB audio recovery");
            nanosleep(&recovery_delay,NULL);
        }
    }
    return NULL;
}
bool ns6_usb_start(void){
    pthread_mutex_lock(&lock); if(thread_created){bool result=initialized;pthread_mutex_unlock(&lock);return result;}
    fraction=0; initialized=false; atomic_store(&completed_transfers,0); atomic_store(&underrun_frames,0); atomic_store(&underrun_events,0); atomic_store(&underrun_max_burst,0); atomic_store(&isoc_packet_errors,0); atomic_store(&isoc_short_packets,0); atomic_store(&isoc_short_bytes,0); atomic_store(&isoc_transfer_errors,0); atomic_store(&last_underrun_log_ms,0); atomic_store(&last_packet_log_ms,0); atomic_store(&last_packet_status,kIOReturnSuccess); atomic_store(&feedback_samples,0); atomic_store(&feedback_44_frames,0); atomic_store(&feedback_45_frames,0); atomic_store(&feedback_other_values,0); atomic_store(&feedback_packet_errors,0); atomic_store(&feedback_last_log_ms,0); feedback_logged_samples=feedback_logged_44=feedback_logged_45=feedback_logged_other=0; memset(feedback_last_sample,0,sizeof(feedback_last_sample)); ns6_transport_reset(); atomic_store(&output_paused,true); atomic_store(&standby_prime_pending,true); atomic_store(&recovery_requested,false); atomic_store(&running,true);
    if(pthread_create(&thread,NULL,worker,NULL)!=0){atomic_store(&running,false);pthread_mutex_unlock(&lock);return false;}
    thread_created=true; while(!initialized&&atomic_load(&running))pthread_cond_wait(&changed,&lock); bool result=initialized; pthread_mutex_unlock(&lock);
    if(!result){pthread_join(thread,NULL);pthread_mutex_lock(&lock);thread_created=false;pthread_mutex_unlock(&lock);} return result;
}
bool ns6_usb_request_restart(void){
    pthread_mutex_lock(&lock);
    if(!thread_created){pthread_mutex_unlock(&lock);return ns6_usb_start();}
    initialized=false;
    atomic_store_explicit(&buffer_restart_requested,true,memory_order_release);
    atomic_store_explicit(&output_paused,true,memory_order_release);
    atomic_store_explicit(&recovery_requested,true,memory_order_release);
    if(run_loop)CFRunLoopWakeUp(run_loop);
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&lock);
    return true;
}
bool ns6_usb_is_ready(void){pthread_mutex_lock(&lock);bool ready=thread_created&&initialized&&atomic_load_explicit(&running,memory_order_acquire);pthread_mutex_unlock(&lock);return ready;}
void ns6_usb_stop(void){
    pthread_mutex_lock(&lock); if(!thread_created){pthread_mutex_unlock(&lock);return;} atomic_store(&output_paused,true); atomic_store(&running,false); if(run_loop)CFRunLoopWakeUp(run_loop); pthread_mutex_unlock(&lock);
    pthread_join(thread,NULL); pthread_mutex_lock(&lock); thread_created=false; initialized=false; pthread_mutex_unlock(&lock);
}
void ns6_usb_set_paused(bool paused){if(!paused&&atomic_load_explicit(&recovery_requested,memory_order_acquire))return;if(!paused&&atomic_load_explicit(&output_paused,memory_order_acquire))atomic_store_explicit(&awaiting_audio_prefill,true,memory_order_release);if(paused)atomic_store_explicit(&awaiting_audio_prefill,false,memory_order_release);atomic_store_explicit(&output_paused,paused,memory_order_release);}
void ns6_usb_set_startup_buffer_frames(uint32_t frames){atomic_store_explicit(&startup_buffer_frames,frames,memory_order_release);}
void ns6_usb_submit_pcm(const uint8_t *pcm24,uint32_t frames){(void)pcm24;(void)frames;}
