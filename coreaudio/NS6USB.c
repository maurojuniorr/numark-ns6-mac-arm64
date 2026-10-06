#include "NS6USB.h"
#include "NS6Transport.h"
#include "NS6MIDI.h"
#include "NS6ClockControl.h"
#include "NS6USBTiming.h"
#include "NS6USBFrameTimestamps.h"
#include "NS6USBWorkerQoS.h"
#include "NS6USBEndpoint.h"
#include "NS6USBConfig.h"
#include "NS6AudioRecovery.h"
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
#include <mach/mach_time.h>

#define VID 0x15e4
#define PID 0x0079
#define RATE 44100
#define FRAME_BYTES 12
#define PACKETS 32
#define FEEDBACK_PACKETS 32
#define FEEDBACK_MAX_PACKET 64
#define FEEDBACK_SLOT_COUNT 2
#define SLOT_COUNT NS6_USB_OUTPUT_SLOT_COUNT
#define MAX_PACKET 156
#define WAVEFORM_READ_SLOTS 3
#define WAVEFORM_READ_BYTES 10240

struct slot {
    IOUSBLowLatencyIsocFrame *frames;
    unsigned char *data;
    unsigned consecutive_errors;
    uint64_t callback_ns;
    uint64_t last_prefill_ns, last_fill_ns, last_fill_cpu_ns;
    uint64_t last_submit_ns, last_submit_cpu_ns;
};
struct feedback_slot { IOUSBLowLatencyIsocFrame *frames; unsigned char *data; };
struct waveform_slot { unsigned char data[WAVEFORM_READ_BYTES]; };
static IOUSBInterfaceInterface **interface;
static IOUSBInterfaceInterface **auxiliary_interface;
static CFRunLoopSourceRef source;
static CFRunLoopSourceRef feedback_source;
static CFRunLoopRef run_loop;
static struct slot slots[SLOT_COUNT];
static struct feedback_slot feedback[FEEDBACK_SLOT_COUNT];
static struct waveform_slot waveform[WAVEFORM_READ_SLOTS];
static UInt8 feedback_pipe;
static UInt16 feedback_packet_bytes;
static UInt8 waveform_pipe;
static pthread_t thread;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static atomic_bool running;
static atomic_bool recovery_requested;
static atomic_bool buffer_restart_requested;
static atomic_bool output_paused=true;
static atomic_bool awaiting_audio_prefill;
static atomic_bool waveform_drain_running;
static atomic_uint startup_buffer_frames=256;
static atomic_bool standby_prime_pending;
static atomic_uint_fast64_t completed_transfers;
static NS6USBTiming completion_interval_timing;
static atomic_uint_fast64_t last_completion_callback_ns;
static NS6USBTiming callback_pre_fill_timing;
static NS6USBTiming fill_duration_timing;
static NS6USBTiming fill_cpu_timing;
static NS6USBTiming callback_submit_timing;
static NS6USBTiming submit_api_timing;
static NS6USBTiming submit_api_cpu_timing;
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
static atomic_uint_fast64_t feedback_last_valid_ms;
static atomic_uint_fast64_t waveform_completions;
static atomic_uint_fast64_t waveform_errors;
static atomic_uint_fast64_t waveform_last_log_ms;
static unsigned char feedback_last_sample[3];
static uint64_t feedback_logged_samples, feedback_logged_44, feedback_logged_45, feedback_logged_other;
static bool thread_created, initialized;
static NS6ClockControl clock_control;
static NS6AudioRecovery audio_recovery;
static atomic_uint_fast64_t protective_rebuffer_events;

static uint64_t monotonic_ms(void){struct timespec now;clock_gettime(CLOCK_MONOTONIC,&now);return (uint64_t)now.tv_sec*1000+(uint64_t)now.tv_nsec/1000000;}
static uint64_t monotonic_ns(void){struct timespec now;clock_gettime(CLOCK_MONOTONIC,&now);return (uint64_t)now.tv_sec*1000000000ULL+(uint64_t)now.tv_nsec;}
static uint64_t thread_cpu_ns(void){struct timespec now;if(clock_gettime(CLOCK_THREAD_CPUTIME_ID,&now)!=0)return 0;return (uint64_t)now.tv_sec*1000000000ULL+(uint64_t)now.tv_nsec;}
static void update_max(atomic_uint_fast64_t *value,uint64_t candidate){
    uint_fast64_t previous=atomic_load_explicit(value,memory_order_relaxed);
    while(candidate>previous&&!atomic_compare_exchange_weak_explicit(value,&previous,candidate,memory_order_relaxed,memory_order_relaxed)){}
}
static bool should_log_once_per_second(atomic_uint_fast64_t *last_log){
    uint_fast64_t now=monotonic_ms(),previous=atomic_load_explicit(last_log,memory_order_relaxed);
    return now-previous>=1000&&atomic_compare_exchange_strong_explicit(last_log,&previous,now,memory_order_relaxed,memory_order_relaxed);
}

static int32_t unpack_pcm24(const unsigned char *sample){
    return (int32_t)sample[0]|((int32_t)sample[1]<<8)|
           ((int32_t)(int8_t)sample[2]<<16);
}
static void pack_pcm24(unsigned char *sample,int32_t value){
    sample[0]=(unsigned char)value;
    sample[1]=(unsigned char)(value>>8);
    sample[2]=(unsigned char)(value>>16);
}
static void apply_recovery_fade(unsigned char *output,unsigned frames){
    for(unsigned frame=0;frame<frames;++frame){
        double gain=ns6_audio_recovery_next_gain(&audio_recovery);
        if(gain>=1.0)break;
        for(unsigned channel=0;channel<4;++channel){
            unsigned char *sample=output+frame*FRAME_BYTES+channel*3;
            pack_pcm24(sample,(int32_t)((double)unpack_pcm24(sample)*gain));
        }
    }
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
    if(!ns6_usb_feedback_reader_enabled()){
        os_log(OS_LOG_DEFAULT,"Numark NS6 diagnostic mode: clock feedback endpoint polling disabled; silent isochronous OUT remains active");
        return;
    }
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
        for(unsigned i=0;i<FEEDBACK_SLOT_COUNT;++i){
            IOReturn result=(*feedback_interface)->LowLatencyCreateBuffer(feedback_interface,(void**)&feedback[i].data,FEEDBACK_PACKETS*feedback_packet_bytes,kUSBLowLatencyReadBuffer);
            if(result==kIOReturnSuccess)result=(*feedback_interface)->LowLatencyCreateBuffer(feedback_interface,(void**)&feedback[i].frames,sizeof(*feedback[i].frames)*FEEDBACK_PACKETS,kUSBLowLatencyFrameListBuffer);
            if(result!=kIOReturnSuccess){
                for(unsigned j=0;j<FEEDBACK_SLOT_COUNT;++j){
                    if(feedback[j].data)(*feedback_interface)->LowLatencyDestroyBuffer(feedback_interface,feedback[j].data);
                    if(feedback[j].frames)(*feedback_interface)->LowLatencyDestroyBuffer(feedback_interface,feedback[j].frames);
                }
                memset(feedback,0,sizeof(feedback));feedback_pipe=0;feedback_packet_bytes=0;
                os_log_error(OS_LOG_DEFAULT,"Numark NS6 optional clock feedback reader allocation failed: 0x%08x",(unsigned)result);
                return;
            }
        }
        os_log(OS_LOG_DEFAULT,"Numark NS6 optional clock feedback reader found on interface 1 pipe %u (packet capacity %u, interval %u, %u queued transfers)",pipe,feedback_packet_bytes,interval,FEEDBACK_SLOT_COUNT);
        return;
    }
    os_log(OS_LOG_DEFAULT,"Numark NS6 optional clock feedback endpoint 0x81 is unavailable; audio output remains enabled");
}
static bool prepare_waveform_drain(void){
    UInt8 endpoints=0;
    if(!auxiliary_interface||(*auxiliary_interface)->GetNumEndpoints(auxiliary_interface,&endpoints)!=kIOReturnSuccess)return false;
    for(UInt8 pipe=1;pipe<=endpoints;++pipe){
        UInt8 direction=0,number=0,type=0,interval=0;UInt16 maximum=0;
        if((*auxiliary_interface)->GetPipeProperties(auxiliary_interface,pipe,&direction,&number,&type,&maximum,&interval)!=kIOReturnSuccess)continue;
        if(!ns6_is_waveform_endpoint(direction,number,type))continue;
        waveform_pipe=pipe;
        IOReturn clear=(*auxiliary_interface)->ClearPipeStallBothEnds(auxiliary_interface,pipe);
        if(clear!=kIOReturnSuccess)
            os_log_error(OS_LOG_DEFAULT,"Numark NS6 waveform endpoint initial clear-halt failed: 0x%08x; continuing with asynchronous reads",(unsigned)clear);
        os_log(OS_LOG_DEFAULT,"Numark NS6 waveform drain endpoint 0x86 found on interface 1 pipe %u (max packet %u; %u queued reads x %u bytes)",pipe,maximum,WAVEFORM_READ_SLOTS,WAVEFORM_READ_BYTES);
        return true;
    }
    waveform_pipe=0;
    os_log_error(OS_LOG_DEFAULT,"Numark NS6 waveform drain endpoint 0x86 is unavailable; audio remains enabled without the Linux stability drain");
    return false;
}
static void waveform_read_complete(void *reference,IOReturn status,void *argument);
static IOReturn submit_waveform_read(struct waveform_slot *slot){
    if(!slot||!auxiliary_interface||!waveform_pipe||!atomic_load_explicit(&waveform_drain_running,memory_order_acquire))return kIOReturnNotReady;
    IOReturn result=(*auxiliary_interface)->ReadPipeAsync(auxiliary_interface,waveform_pipe,slot->data,sizeof(slot->data),waveform_read_complete,slot);
    if(result==kIOUSBPipeStalled&&atomic_load_explicit(&waveform_drain_running,memory_order_acquire)){
        IOReturn clear=(*auxiliary_interface)->ClearPipeStallBothEnds(auxiliary_interface,waveform_pipe);
        if(clear==kIOReturnSuccess)
            result=(*auxiliary_interface)->ReadPipeAsync(auxiliary_interface,waveform_pipe,slot->data,sizeof(slot->data),waveform_read_complete,slot);
    }
    return result;
}
static void waveform_read_complete(void *reference,IOReturn status,void *argument){
    (void)argument;
    struct waveform_slot *slot=reference;
    if(!atomic_load_explicit(&waveform_drain_running,memory_order_acquire))return;
    if(status==kIOReturnSuccess){
        uint64_t completed=atomic_fetch_add_explicit(&waveform_completions,1,memory_order_relaxed)+1;
        if(completed%10000==0)
            os_log(OS_LOG_DEFAULT,"Numark NS6 waveform endpoint drain active: %llu reads, %llu read errors",(unsigned long long)completed,(unsigned long long)atomic_load_explicit(&waveform_errors,memory_order_relaxed));
    }else{
        uint64_t errors=atomic_fetch_add_explicit(&waveform_errors,1,memory_order_relaxed)+1;
        if(should_log_once_per_second(&waveform_last_log_ms))
            os_log_error(OS_LOG_DEFAULT,"Numark NS6 waveform endpoint read failed: 0x%08x (%llu errors)",(unsigned)status,(unsigned long long)errors);
        if(status==kIOUSBPipeStalled&&atomic_load_explicit(&waveform_drain_running,memory_order_acquire)){
            IOReturn clear=(*auxiliary_interface)->ClearPipeStallBothEnds(auxiliary_interface,waveform_pipe);
            if(clear!=kIOReturnSuccess&&should_log_once_per_second(&waveform_last_log_ms))
                os_log_error(OS_LOG_DEFAULT,"Numark NS6 waveform endpoint stall recovery failed: 0x%08x",(unsigned)clear);
        }
    }
    IOReturn result=submit_waveform_read(slot);
    if(result!=kIOReturnSuccess){
        uint64_t errors=atomic_fetch_add_explicit(&waveform_errors,1,memory_order_relaxed)+1;
        if(should_log_once_per_second(&waveform_last_log_ms))
            os_log_error(OS_LOG_DEFAULT,"Numark NS6 waveform endpoint read resubmit failed: 0x%08x (%llu errors)",(unsigned)result,(unsigned long long)errors);
    }
}
static void start_waveform_drain(void){
    if(!waveform_pipe)return;
    atomic_store_explicit(&waveform_drain_running,true,memory_order_release);
    unsigned queued=0;
    for(unsigned i=0;i<WAVEFORM_READ_SLOTS;++i){
        IOReturn result=submit_waveform_read(&waveform[i]);
        if(result==kIOReturnSuccess)++queued;
        else{
            uint64_t errors=atomic_fetch_add_explicit(&waveform_errors,1,memory_order_relaxed)+1;
            os_log_error(OS_LOG_DEFAULT,"Numark NS6 waveform endpoint initial read %u failed: 0x%08x (%llu errors)",i,(unsigned)result,(unsigned long long)errors);
        }
    }
    os_log(OS_LOG_DEFAULT,"Numark NS6 waveform drain started: %u/%u asynchronous reads queued",queued,WAVEFORM_READ_SLOTS);
}
static void feedback_submit(struct feedback_slot *slot);
static void feedback_complete(void *reference,IOReturn status,void *argument){
    (void)argument;
    struct feedback_slot *slot=reference;
    if(!atomic_load_explicit(&running,memory_order_acquire))return;
    if(status!=kIOReturnSuccess){
        uint64_t errors=atomic_fetch_add_explicit(&feedback_packet_errors,1,memory_order_relaxed)+1;
        if(should_log_once_per_second(&feedback_last_log_ms))
            os_log_error(OS_LOG_DEFAULT,"Numark NS6 feedback endpoint read failed: 0x%08x (%llu errors)",(unsigned)status,(unsigned long long)errors);
        feedback_submit(slot);return;
    }
    for(unsigned p=0;p<FEEDBACK_PACKETS;++p){
        IOUSBLowLatencyIsocFrame *frame=&slot->frames[p];
        if(frame->frStatus!=kIOReturnSuccess){atomic_fetch_add_explicit(&feedback_packet_errors,1,memory_order_relaxed);continue;}
        if(frame->frActCount<3)continue;
        const unsigned char *packet=slot->data+p*feedback_packet_bytes;
        feedback_last_sample[0]=packet[0];feedback_last_sample[1]=packet[1];feedback_last_sample[2]=packet[2];
        atomic_fetch_add_explicit(&feedback_samples,1,memory_order_relaxed);
        bool was_locked=ns6_clock_control_is_locked(&clock_control);
        if(packet[0]==44){atomic_fetch_add_explicit(&feedback_44_frames,1,memory_order_relaxed);ns6_clock_control_observe(&clock_control,packet[0]);}
        else if(packet[0]==45){atomic_fetch_add_explicit(&feedback_45_frames,1,memory_order_relaxed);ns6_clock_control_observe(&clock_control,packet[0]);}
        else atomic_fetch_add_explicit(&feedback_other_values,1,memory_order_relaxed);
        if(packet[0]==44||packet[0]==45)
            atomic_store_explicit(&feedback_last_valid_ms,monotonic_ms(),memory_order_release);
        if(!was_locked&&ns6_clock_control_is_locked(&clock_control))
            os_log(OS_LOG_DEFAULT,"Numark NS6 USB feedback active: %.3f Hz diagnostic; clock mode %{public}s",(double)ns6_clock_control_rate_millihz(&clock_control)/1000.0,ns6_clock_control_is_adaptive_enabled()?"adaptive":"fixed nominal (diagnostic only)");
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
        os_log(OS_LOG_DEFAULT,"Numark NS6 USB clock feedback: %llu samples, 44=%llu, 45=%llu, other=%llu, mean %.4f frames/report; mode %{public}s; diagnostic feedback %.3f Hz; output rate %.3f Hz; source step %.8f; request total %llu, sent %llu, debt %lld; queue %u frames; cumulative %llu samples; last %02x %02x %02x; read errors %llu",(unsigned long long)window_samples,(unsigned long long)window44,(unsigned long long)window45,(unsigned long long)window_other,average,ns6_clock_control_is_adaptive_enabled()?(ns6_clock_control_is_locked(&clock_control)?"adaptive":"adaptive warmup"):"fixed nominal (diagnostic)",(double)ns6_clock_control_rate_millihz(&clock_control)/1000.0,(double)ns6_clock_control_output_rate_millihz(&clock_control)/1000.0,ns6_clock_control_source_frames_per_output_frame(&clock_control,ns6_transport_available()),(unsigned long long)ns6_clock_control_requested_total(&clock_control),(unsigned long long)ns6_clock_control_sent_total(&clock_control),(long long)ns6_clock_control_debt(&clock_control),ns6_transport_available(),(unsigned long long)samples,feedback_last_sample[0],feedback_last_sample[1],feedback_last_sample[2],(unsigned long long)atomic_load_explicit(&feedback_packet_errors,memory_order_relaxed));
        feedback_logged_samples=samples;feedback_logged_44=count44;feedback_logged_45=count45;feedback_logged_other=other;
    }
    feedback_submit(slot);
}
static void feedback_submit(struct feedback_slot *slot){
    if(!slot||!feedback_pipe||!slot->data||!slot->frames||!atomic_load_explicit(&running,memory_order_acquire))return;
    for(unsigned p=0;p<FEEDBACK_PACKETS;++p){slot->frames[p].frReqCount=feedback_packet_bytes;slot->frames[p].frActCount=0;slot->frames[p].frStatus=0;}
    IOReturn result=(*auxiliary_interface)->LowLatencyReadIsochPipeAsync(auxiliary_interface,feedback_pipe,slot->data,0,FEEDBACK_PACKETS,1,slot->frames,feedback_complete,slot);
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
    unsigned millisecond_frames[8];
    for(unsigned packet=0;packet<PACKETS;++packet){
        uint64_t last_feedback=atomic_load_explicit(&feedback_last_valid_ms,memory_order_acquire);
        if(ns6_clock_control_is_locked(&clock_control)&&last_feedback&&monotonic_ms()-last_feedback>2000){
            ns6_clock_control_reset_feedback(&clock_control);
            atomic_store_explicit(&feedback_last_valid_ms,0,memory_order_release);
            os_log_error(OS_LOG_DEFAULT,"Numark NS6 USB clock feedback stale; discarding its estimate and retaining nominal 44.1 kHz cadence");
        }
        if((packet % 8u) == 0)
            ns6_clock_control_next_millisecond(&clock_control, millisecond_frames);
        unsigned frames=millisecond_frames[packet % 8u];
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
            UInt32 startup_frames=atomic_load_explicit(&startup_buffer_frames,memory_order_acquire);
            if(startup_frames<NS6_RECOVERY_RESUME_FRAMES)startup_frames=NS6_RECOVERY_RESUME_FRAMES;
            if(ns6_transport_available()<startup_frames){
                memset(output,0,frames*FRAME_BYTES);
                output+=frames*FRAME_BYTES;
                continue;
            }
            atomic_store_explicit(&awaiting_audio_prefill,false,memory_order_release);
            ns6_audio_recovery_init(&audio_recovery);
        }
        UInt32 available=ns6_transport_available();
        bool was_buffering=audio_recovery.buffering;
        if(ns6_audio_recovery_needs_silence(&audio_recovery,available)){
            if(!was_buffering)atomic_fetch_add_explicit(&protective_rebuffer_events,1,memory_order_relaxed);
            memset(output,0,frames*FRAME_BYTES);
            output+=frames*FRAME_BYTES;
            continue;
        }
        double source_step=ns6_clock_control_source_frames_per_output_frame(&clock_control,available);
        UInt32 received=ns6_transport_dequeue_resampled(output,frames,source_step);
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
            ns6_transport_advance_resampled((UInt32)missing,source_step);
        }
        apply_recovery_fade(output,received);
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
    slot->callback_ns=monotonic_ns();
    uint64_t previous_callback_ns=atomic_exchange_explicit(&last_completion_callback_ns,slot->callback_ns,memory_order_relaxed);
    uint64_t callback_gap_ns=previous_callback_ns&&slot->callback_ns>previous_callback_ns?slot->callback_ns-previous_callback_ns:0;
    ns6_usb_timing_record(&completion_interval_timing,slot->callback_ns);
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
    if(ns6_usb_timing_is_callback_gap(callback_gap_ns)){
        uint64_t last_feedback=atomic_load_explicit(&feedback_last_valid_ms,memory_order_acquire);
        uint64_t now_ms=monotonic_ms();
        uint64_t feedback_age_ms=last_feedback&&now_ms>=last_feedback?now_ms-last_feedback:0;
        NS6USBFrameTimestampRange timestamp_range=ns6_usb_frame_timestamp_range(slot->frames,PACKETS);
        mach_timebase_info_data_t timebase={0};
        uint64_t newest_age_us=0,span_us=0;
        if(timestamp_range.valid_frames&&mach_timebase_info(&timebase)==KERN_SUCCESS&&timebase.denom){
            uint64_t now_ticks=mach_absolute_time();
            uint64_t age_ticks=now_ticks>=timestamp_range.newest_ticks?now_ticks-timestamp_range.newest_ticks:0;
            uint64_t span_ticks=timestamp_range.newest_ticks-timestamp_range.oldest_ticks;
            newest_age_us=(uint64_t)(((long double)age_ticks*timebase.numer)/(timebase.denom*1000.0L));
            span_us=(uint64_t)(((long double)span_ticks*timebase.numer)/(timebase.denom*1000.0L));
        }
        os_log(OS_LOG_DEFAULT,"Numark NS6 USB callback gap: %llu us; slot %u; transfer 0x%08x; bad frames %u (last 0x%08x); short frames %u; queue %u frames; paused %u; feedback age %llu ms, samples %llu, errors %llu; timestamps %u/%u span %llu us newest age %llu us; previous submit wall/cpu %llu/%llu us; prefill %llu us; fill wall/cpu %llu/%llu us",
            (unsigned long long)(callback_gap_ns/1000),(unsigned)(slot-slots),(unsigned)status,packet_errors,(unsigned)packet_error_status,short_packets,ns6_transport_available(),atomic_load_explicit(&output_paused,memory_order_acquire)?1u:0u,(unsigned long long)feedback_age_ms,(unsigned long long)atomic_load_explicit(&feedback_samples,memory_order_relaxed),(unsigned long long)atomic_load_explicit(&feedback_packet_errors,memory_order_relaxed),(unsigned)timestamp_range.valid_frames,(unsigned)PACKETS,(unsigned long long)span_us,(unsigned long long)newest_age_us,(unsigned long long)(slot->last_submit_ns/1000),(unsigned long long)(slot->last_submit_cpu_ns/1000),(unsigned long long)(slot->last_prefill_ns/1000),(unsigned long long)(slot->last_fill_ns/1000),(unsigned long long)(slot->last_fill_cpu_ns/1000));
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
    if(completed%2500==0){
        os_log(OS_LOG_DEFAULT,"Numark NS6 audio health: transfers %llu, underruns %llu events/%llu frames (max burst %llu), protective rebuffer events %llu, USB transfer errors %llu, packet errors %llu (last 0x%08x), short packets %llu/%llu bytes, queue %u frames",(unsigned long long)completed,(unsigned long long)atomic_load_explicit(&underrun_events,memory_order_relaxed),(unsigned long long)atomic_load_explicit(&underrun_frames,memory_order_relaxed),(unsigned long long)atomic_load_explicit(&underrun_max_burst,memory_order_relaxed),(unsigned long long)atomic_load_explicit(&protective_rebuffer_events,memory_order_relaxed),(unsigned long long)atomic_load_explicit(&isoc_transfer_errors,memory_order_relaxed),(unsigned long long)atomic_load_explicit(&isoc_packet_errors,memory_order_relaxed),(unsigned)atomic_load_explicit(&last_packet_status,memory_order_relaxed),(unsigned long long)atomic_load_explicit(&isoc_short_packets,memory_order_relaxed),(unsigned long long)atomic_load_explicit(&isoc_short_bytes,memory_order_relaxed),ns6_transport_available());
        NS6USBTimingSnapshot cadence={0},pre_fill={0},fill_work={0},fill_cpu={0},work={0},submit_api={0},submit_api_cpu={0};
        if(ns6_usb_timing_take_snapshot(&completion_interval_timing,&cadence))
            os_log(OS_LOG_DEFAULT,"Numark NS6 USB cadence: %llu intervals, mean %.1f us, min %llu us, max %llu us",(unsigned long long)cadence.intervals,(double)cadence.total_ns/(double)cadence.intervals/1000.0,(unsigned long long)(cadence.min_ns/1000),(unsigned long long)(cadence.max_ns/1000));
        if(ns6_usb_timing_take_snapshot(&callback_pre_fill_timing,&pre_fill))
            os_log(OS_LOG_DEFAULT,"Numark NS6 USB callback before fill: %llu intervals, mean %.1f us, min %llu us, max %llu us",(unsigned long long)pre_fill.intervals,(double)pre_fill.total_ns/(double)pre_fill.intervals/1000.0,(unsigned long long)(pre_fill.min_ns/1000),(unsigned long long)(pre_fill.max_ns/1000));
        if(ns6_usb_timing_take_snapshot(&fill_duration_timing,&fill_work))
            os_log(OS_LOG_DEFAULT,"Numark NS6 USB fill duration: %llu intervals, mean %.1f us, min %llu us, max %llu us",(unsigned long long)fill_work.intervals,(double)fill_work.total_ns/(double)fill_work.intervals/1000.0,(unsigned long long)(fill_work.min_ns/1000),(unsigned long long)(fill_work.max_ns/1000));
        if(ns6_usb_timing_take_snapshot(&fill_cpu_timing,&fill_cpu))
            os_log(OS_LOG_DEFAULT,"Numark NS6 USB fill thread CPU: %llu intervals, mean %.1f us, min %llu us, max %llu us",(unsigned long long)fill_cpu.intervals,(double)fill_cpu.total_ns/(double)fill_cpu.intervals/1000.0,(unsigned long long)(fill_cpu.min_ns/1000),(unsigned long long)(fill_cpu.max_ns/1000));
        if(ns6_usb_timing_take_snapshot(&callback_submit_timing,&work))
            os_log(OS_LOG_DEFAULT,"Numark NS6 USB callback work: %llu intervals, mean %.1f us, min %llu us, max %llu us",(unsigned long long)work.intervals,(double)work.total_ns/(double)work.intervals/1000.0,(unsigned long long)(work.min_ns/1000),(unsigned long long)(work.max_ns/1000));
        if(ns6_usb_timing_take_snapshot(&submit_api_timing,&submit_api))
            os_log(OS_LOG_DEFAULT,"Numark NS6 USB submit API: %llu intervals, mean %.1f us, min %llu us, max %llu us",(unsigned long long)submit_api.intervals,(double)submit_api.total_ns/(double)submit_api.intervals/1000.0,(unsigned long long)(submit_api.min_ns/1000),(unsigned long long)(submit_api.max_ns/1000));
        if(ns6_usb_timing_take_snapshot(&submit_api_cpu_timing,&submit_api_cpu))
            os_log(OS_LOG_DEFAULT,"Numark NS6 USB submit API thread CPU: %llu intervals, mean %.1f us, min %llu us, max %llu us",(unsigned long long)submit_api_cpu.intervals,(double)submit_api_cpu.total_ns/(double)submit_api_cpu.intervals/1000.0,(unsigned long long)(submit_api_cpu.min_ns/1000),(unsigned long long)(submit_api_cpu.max_ns/1000));
    }
    submit(slot);
}
static void submit(struct slot *slot){
    if(!atomic_load_explicit(&running,memory_order_acquire)||atomic_load_explicit(&recovery_requested,memory_order_acquire))return;
    uint64_t callback_ns=slot->callback_ns;
    uint64_t fill_start=monotonic_ns();
    uint64_t fill_cpu_start=thread_cpu_ns();
    slot->last_prefill_ns=callback_ns&&fill_start>=callback_ns?fill_start-callback_ns:0;
    if(callback_ns)ns6_usb_timing_record_duration(&callback_pre_fill_timing,fill_start-callback_ns);
    fill(slot);
    uint64_t fill_end=monotonic_ns();
    uint64_t fill_cpu_end=thread_cpu_ns();
    slot->last_fill_ns=fill_end>=fill_start?fill_end-fill_start:0;
    slot->last_fill_cpu_ns=fill_cpu_start&&fill_cpu_end>=fill_cpu_start?fill_cpu_end-fill_cpu_start:0;
    ns6_usb_timing_record_duration(&fill_duration_timing,fill_end-fill_start);
    if(fill_cpu_start&&fill_cpu_end>=fill_cpu_start)ns6_usb_timing_record_duration(&fill_cpu_timing,fill_cpu_end-fill_cpu_start);
    if(callback_ns){ns6_usb_timing_record_duration(&callback_submit_timing,fill_end-callback_ns);slot->callback_ns=0;}
    uint64_t submit_start=monotonic_ns();
    uint64_t submit_cpu_start=thread_cpu_ns();
    IOReturn result=(*interface)->LowLatencyWriteIsochPipeAsync(interface,1,slot->data,0,PACKETS,1,slot->frames,complete,slot);
    uint64_t submit_end=monotonic_ns();
    uint64_t submit_cpu_end=thread_cpu_ns();
    slot->last_submit_ns=submit_end>=submit_start?submit_end-submit_start:0;
    slot->last_submit_cpu_ns=submit_cpu_start&&submit_cpu_end>=submit_cpu_start?submit_cpu_end-submit_cpu_start:0;
    ns6_usb_timing_record_duration(&submit_api_timing,submit_end-submit_start);
    if(submit_cpu_start&&submit_cpu_end>=submit_cpu_start)ns6_usb_timing_record_duration(&submit_api_cpu_timing,submit_cpu_end-submit_cpu_start);
    if(result!=kIOReturnSuccess)request_recovery("submit isochronous transfer",result);
}
static bool initialize(void){
    unsigned char capability[64]={0},rate[3]={0x44,0xac,0}; interface=open_interface(); if(!interface){os_log_error(OS_LOG_DEFAULT,"Numark NS6 could not find USB interface 0");return false;}
    IOReturn result=(*interface)->USBInterfaceOpenSeize(interface); if(result!=kIOReturnSuccess)return usb_failure("open interface",result);
    auxiliary_interface=open_interface_once(1); if(!auxiliary_interface){os_log_error(OS_LOG_DEFAULT,"Numark NS6 could not find USB interface 1");return false;}
    result=(*auxiliary_interface)->USBInterfaceOpenSeize(auxiliary_interface); if(result!=kIOReturnSuccess)return usb_failure("open auxiliary interface",result);
    result=(*auxiliary_interface)->SetAlternateInterface(auxiliary_interface,1); if(result!=kIOReturnSuccess)return usb_failure("select auxiliary alternate setting",result);
    (void)prepare_waveform_drain();
    result=(*interface)->CreateInterfaceAsyncEventSource(interface,&source); if(result!=kIOReturnSuccess||!source)return usb_failure("create async source",result);
    run_loop=CFRunLoopGetCurrent(); CFRetain(run_loop); CFRunLoopAddSource(run_loop,source,kCFRunLoopDefaultMode);
    if(ns6_usb_feedback_reader_enabled()){
        result=(*auxiliary_interface)->CreateInterfaceAsyncEventSource(auxiliary_interface,&feedback_source); if(result!=kIOReturnSuccess||!feedback_source)return usb_failure("create feedback async source",result);
        CFRunLoopAddSource(run_loop,feedback_source,kCFRunLoopDefaultMode);
    }
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
    atomic_store_explicit(&waveform_drain_running,false,memory_order_release);
    if(interface)(*interface)->AbortPipe(interface,1);if(auxiliary_interface&&feedback_pipe)(*auxiliary_interface)->AbortPipe(auxiliary_interface,feedback_pipe);if(auxiliary_interface&&waveform_pipe)(*auxiliary_interface)->AbortPipe(auxiliary_interface,waveform_pipe); if(run_loop)CFRunLoopRunInMode(kCFRunLoopDefaultMode,0.2,false);
    if(auxiliary_interface)for(unsigned i=0;i<FEEDBACK_SLOT_COUNT;++i){if(feedback[i].data)(*auxiliary_interface)->LowLatencyDestroyBuffer(auxiliary_interface,feedback[i].data);if(feedback[i].frames)(*auxiliary_interface)->LowLatencyDestroyBuffer(auxiliary_interface,feedback[i].frames);} memset(feedback,0,sizeof(feedback));feedback_pipe=0;feedback_packet_bytes=0;
    waveform_pipe=0;
    if(interface)for(unsigned i=0;i<SLOT_COUNT;++i){if(slots[i].data)(*interface)->LowLatencyDestroyBuffer(interface,slots[i].data);if(slots[i].frames)(*interface)->LowLatencyDestroyBuffer(interface,slots[i].frames);} memset(slots,0,sizeof(slots));
    if(run_loop&&source)CFRunLoopRemoveSource(run_loop,source,kCFRunLoopDefaultMode); if(source)CFRelease(source); source=NULL;
    if(run_loop&&feedback_source)CFRunLoopRemoveSource(run_loop,feedback_source,kCFRunLoopDefaultMode); if(feedback_source)CFRelease(feedback_source); feedback_source=NULL;
    if(run_loop)CFRelease(run_loop); run_loop=NULL;
    if(auxiliary_interface){(*auxiliary_interface)->USBInterfaceClose(auxiliary_interface);(*auxiliary_interface)->Release(auxiliary_interface);} auxiliary_interface=NULL;
    if(interface){(*interface)->USBInterfaceClose(interface);(*interface)->Release(interface);} interface=NULL;
}
static void *worker(void *unused){
    (void)unused;
    qos_class_t initial_qos=QOS_CLASS_UNSPECIFIED,effective_qos=QOS_CLASS_UNSPECIFIED;
    int initial_priority=0,effective_priority=0;
    (void)pthread_get_qos_class_np(pthread_self(),&initial_qos,&initial_priority);
    bool qos_updated=ns6_usb_worker_set_interactive_qos(&effective_qos,&effective_priority);
    os_log(OS_LOG_DEFAULT,"Numark NS6 USB worker QoS: initial class %u priority %d; requested user-interactive, verified %d (class %u priority %d)",(unsigned)initial_qos,initial_priority,qos_updated?1:0,(unsigned)effective_qos,effective_priority);
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
        atomic_store_explicit(&waveform_completions,0,memory_order_relaxed);atomic_store_explicit(&waveform_errors,0,memory_order_relaxed);atomic_store_explicit(&waveform_last_log_ms,0,memory_order_relaxed);
        for(unsigned i=0;i<SLOT_COUNT;++i)submit(&slots[i]);
        os_log(OS_LOG_DEFAULT,"Numark NS6 diagnostic USB OUT queue: %u transfers x %u microframes (%u queued microframes); feedback polling %{public}s",(unsigned)SLOT_COUNT,(unsigned)PACKETS,(unsigned)(SLOT_COUNT*PACKETS),ns6_usb_feedback_reader_enabled()?"enabled":"disabled");
        for(unsigned i=0;i<FEEDBACK_SLOT_COUNT;++i)feedback_submit(&feedback[i]);
        bool midi_session_active=ns6_midi_start(interface,run_loop);
        if(midi_session_active){
            (void)ns6_midi_handshake(run_loop);
            if(ns6_usb_midi_input_enabled())
                os_log(OS_LOG_DEFAULT,"Numark NS6 waveform endpoint drain disabled during active MIDI session");
            else
                os_log(OS_LOG_DEFAULT,"Numark NS6 diagnostic audio-only run completed outbound activation; input polling and waveform drain disabled");
        }else{
            fprintf(stderr,"Numark NS6 MIDI unavailable; continuing with audio only\n");
            if(ns6_should_run_waveform_drain(midi_session_active)){
                os_log(OS_LOG_DEFAULT,"Numark NS6 MIDI unavailable; enabling waveform endpoint drain for audio-only session");
                start_waveform_drain();
            }
        }
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
    ns6_clock_control_init(&clock_control); ns6_audio_recovery_init(&audio_recovery); ns6_usb_timing_init(&completion_interval_timing); atomic_store(&last_completion_callback_ns,0); ns6_usb_timing_init(&callback_pre_fill_timing); ns6_usb_timing_init(&fill_duration_timing); ns6_usb_timing_init(&fill_cpu_timing); ns6_usb_timing_init(&callback_submit_timing); ns6_usb_timing_init(&submit_api_timing); ns6_usb_timing_init(&submit_api_cpu_timing); initialized=false; atomic_store(&completed_transfers,0); atomic_store(&underrun_frames,0); atomic_store(&underrun_events,0); atomic_store(&underrun_max_burst,0); atomic_store(&protective_rebuffer_events,0); atomic_store(&isoc_packet_errors,0); atomic_store(&isoc_short_packets,0); atomic_store(&isoc_short_bytes,0); atomic_store(&isoc_transfer_errors,0); atomic_store(&last_underrun_log_ms,0); atomic_store(&last_packet_log_ms,0); atomic_store(&last_packet_status,kIOReturnSuccess); atomic_store(&feedback_samples,0); atomic_store(&feedback_44_frames,0); atomic_store(&feedback_45_frames,0); atomic_store(&feedback_other_values,0); atomic_store(&feedback_packet_errors,0); atomic_store(&feedback_last_log_ms,0); atomic_store(&feedback_last_valid_ms,0); feedback_logged_samples=feedback_logged_44=feedback_logged_45=feedback_logged_other=0; memset(feedback_last_sample,0,sizeof(feedback_last_sample)); ns6_transport_reset(); atomic_store(&output_paused,true); atomic_store(&standby_prime_pending,true); atomic_store(&recovery_requested,false); atomic_store(&running,true);
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
