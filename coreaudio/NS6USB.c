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
#include "NS6AudioPrefill.h"
#include "NS6FirmwareVersion.h"
#include "NS6IOMetrics.h"
#include "NS6FeedbackTelemetry.h"
#include "NS6FeedbackReadGate.h"
#include "NS6IdlePCM.h"
#include "NS6CaptureDecoder.h"
#include "NS6CaptureRing.h"
#include "NS6BulkINIdlePolicy.h"
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
#define NS6_STARTUP_MIN_PREFILL_FRAMES 1024u
#ifndef NS6_USB_LOG_EXCLUSIVE_OWNER
#define NS6_USB_LOG_EXCLUSIVE_OWNER 0
#endif
#ifndef NS6_IDLE_PCM_KEEPALIVE
#define NS6_IDLE_PCM_KEEPALIVE 0
#endif
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
struct waveform_slot { unsigned char data[WAVEFORM_READ_BYTES]; atomic_bool in_flight; uint32_t generation; };
static IOUSBInterfaceInterface **interface;
static IOUSBInterfaceInterface **auxiliary_interface;
static CFRunLoopSourceRef source;
static CFRunLoopSourceRef feedback_source;
static CFRunLoopRef run_loop;
static struct slot slots[SLOT_COUNT];
static struct feedback_slot feedback[FEEDBACK_SLOT_COUNT];
static atomic_bool feedback_in_flight[FEEDBACK_SLOT_COUNT];
static struct waveform_slot waveform[WAVEFORM_READ_SLOTS];
static NS6CaptureRing local_capture_ring;
static NS6CaptureRing *capture_ring=&local_capture_ring;
static bool shared_capture_ring;
static atomic_bool *shared_capture_requested;
static atomic_uint_fast64_t *shared_capture_last_demand_ns;
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
static atomic_uint waveform_generation;
static atomic_bool capture_active;
static atomic_uint_fast64_t capture_last_demand_ns;
static atomic_uint_fast64_t capture_frames_decoded;
static atomic_uint_fast64_t capture_ring_drops;
static atomic_uint startup_buffer_frames=256;
static atomic_uint firmware_response_bits;
static atomic_bool firmware_response_valid;
static atomic_bool standby_prime_pending;
static atomic_uint_fast64_t completed_transfers;
static NS6USBTiming completion_interval_timing;
static NS6USBTiming completion_dispatch_lag_timing;
static NS6USBTiming feedback_completion_interval_timing;
static NS6USBTiming feedback_completion_dispatch_lag_timing;
static mach_timebase_info_data_t usb_timebase;
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
static atomic_uint_fast64_t isoc_60_byte_packets;
static atomic_uint_fast64_t isoc_72_byte_packets;
static atomic_uint_fast64_t isoc_other_size_packets;
static atomic_uint_fast64_t isoc_transfer_errors;
static atomic_uint_fast64_t last_underrun_log_ms;
static atomic_uint_fast64_t last_packet_log_ms;
static atomic_int last_packet_status;
static atomic_uint_fast64_t feedback_samples;
static atomic_uint_fast64_t feedback_44_frames;
static atomic_uint_fast64_t feedback_45_frames;
static atomic_uint_fast64_t feedback_other_values;
static atomic_uint_fast64_t feedback_short_packets;
static atomic_uint_fast64_t feedback_out_of_range_values;
static atomic_uint_fast64_t feedback_lock_events;
static atomic_uint_fast64_t feedback_stale_resets;
static atomic_uint_fast64_t feedback_packet_errors;
static atomic_uint_fast64_t feedback_last_log_ms;
static atomic_uint_fast64_t feedback_short_log_ms;
static atomic_uint_fast64_t feedback_invalid_log_ms;
static atomic_uint_fast64_t feedback_transport_log_ms;
static atomic_uint_fast64_t feedback_last_valid_ms;
static atomic_uint_fast64_t feedback_stale_reset_started_ms;
static atomic_uint_fast64_t waveform_completions;
static atomic_uint_fast64_t waveform_errors;
static atomic_uint_fast64_t waveform_last_log_ms;
static unsigned char feedback_last_sample[3];
static uint64_t feedback_logged_samples, feedback_logged_44, feedback_logged_45, feedback_logged_other;
static uint64_t feedback_logged_short, feedback_logged_out_of_range;
static uint64_t feedback_logged_lock_events, feedback_logged_stale_resets;
static bool thread_created, initialized;
static NS6ClockControl clock_control;
static int32_t last_recovery_sample[4];
static bool have_last_recovery_sample;

static uint64_t monotonic_ms(void){struct timespec now;clock_gettime(CLOCK_MONOTONIC,&now);return (uint64_t)now.tv_sec*1000+(uint64_t)now.tv_nsec/1000000;}
static uint64_t monotonic_ns(void){struct timespec now;clock_gettime(CLOCK_MONOTONIC,&now);return (uint64_t)now.tv_sec*1000000000ULL+(uint64_t)now.tv_nsec;}
static uint64_t mach_ticks_to_ns(uint64_t ticks){if(!usb_timebase.denom)return 0;return (ticks/usb_timebase.denom)*usb_timebase.numer+((ticks%usb_timebase.denom)*usb_timebase.numer)/usb_timebase.denom;}
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
static void log_ns6_interface_owner(UInt8 wanted_number,IOReturn result){
#if NS6_USB_LOG_EXCLUSIVE_OWNER
    io_iterator_t iterator=IO_OBJECT_NULL;io_service_t service;bool found=false;
    if(IOServiceGetMatchingServices(kIOMainPortDefault,IOServiceMatching("IOUSBHostInterface"),&iterator)!=kIOReturnSuccess)return;
    while((service=IOIteratorNext(iterator))){
        UInt8 number=255;
        if(!is_ns6_interface(service)){
            IOObjectRelease(service);continue;
        }
        IOCFPlugInInterface **plugin=NULL;IOUSBInterfaceInterface **candidate=NULL;SInt32 score=0;
        HRESULT query=IOCreatePlugInInterfaceForService(service,kIOUSBInterfaceUserClientTypeID,kIOCFPlugInInterfaceID,&plugin,&score);
        if(query==kIOReturnSuccess&&plugin){query=(*plugin)->QueryInterface(plugin,CFUUIDGetUUIDBytes(kIOUSBInterfaceInterfaceID),(LPVOID)&candidate);IODestroyPlugInInterface(plugin);}
        if(query==kIOReturnSuccess&&candidate)(*candidate)->GetInterfaceNumber(candidate,&number);
        if(number==wanted_number){
            found=true;char owner_text[256]="unavailable";io_name_t class_name="unknown";
            CFTypeRef owner=IORegistryEntryCreateCFProperty(service,CFSTR("UsbExclusiveOwner"),kCFAllocatorDefault,0);
            if(owner&&CFGetTypeID(owner)==CFStringGetTypeID())CFStringGetCString((CFStringRef)owner,owner_text,sizeof(owner_text),kCFStringEncodingUTF8);
            (void)IOObjectGetClass(service,class_name);
            os_log_error(OS_LOG_DEFAULT,"Numark NS6 USB interface exclusive-open diagnostic: interface %u, result 0x%08x, current owner %{public}s, service class %{public}s",wanted_number,(unsigned)result,owner_text,class_name);
            if(owner)CFRelease(owner);
            if(candidate)(*candidate)->Release(candidate);
            IOObjectRelease(service);break;
        }
        if(candidate)(*candidate)->Release(candidate);
        IOObjectRelease(service);
    }
    IOObjectRelease(iterator);
    if(!found)os_log_error(OS_LOG_DEFAULT,"Numark NS6 USB interface exclusive-open diagnostic: interface %u, result 0x%08x, owner unavailable (interface not found in IORegistry)",wanted_number,(unsigned)result);
#else
    (void)wanted_number;(void)result;
#endif
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
        os_log(OS_LOG_DEFAULT,"Numark NS6 bulk-IN endpoint 0x86 found on interface 1 pipe %u (max packet %u; %u queued reads x %u bytes; capture decoder ready)",pipe,maximum,WAVEFORM_READ_SLOTS,WAVEFORM_READ_BYTES);
        return true;
    }
    waveform_pipe=0;
    os_log_error(OS_LOG_DEFAULT,"Numark NS6 waveform drain endpoint 0x86 is unavailable; audio remains enabled without the Linux stability drain");
    return false;
}
static void waveform_read_complete(void *reference,IOReturn status,void *argument);
static IOReturn submit_waveform_read(struct waveform_slot *slot){
    if(!slot||!auxiliary_interface||!waveform_pipe||!atomic_load_explicit(&waveform_drain_running,memory_order_acquire))return kIOReturnNotReady;
    bool expected=false;
    if(!atomic_compare_exchange_strong_explicit(&slot->in_flight,&expected,true,memory_order_acq_rel,memory_order_acquire))return kIOReturnBusy;
    slot->generation=atomic_load_explicit(&waveform_generation,memory_order_acquire);
    IOReturn result=(*auxiliary_interface)->ReadPipeAsync(auxiliary_interface,waveform_pipe,slot->data,sizeof(slot->data),waveform_read_complete,slot);
    if(result==kIOUSBPipeStalled&&atomic_load_explicit(&waveform_drain_running,memory_order_acquire)){
        IOReturn clear=(*auxiliary_interface)->ClearPipeStallBothEnds(auxiliary_interface,waveform_pipe);
        if(clear==kIOReturnSuccess)
            result=(*auxiliary_interface)->ReadPipeAsync(auxiliary_interface,waveform_pipe,slot->data,sizeof(slot->data),waveform_read_complete,slot);
    }
    if(result!=kIOReturnSuccess)atomic_store_explicit(&slot->in_flight,false,memory_order_release);
    return result;
}
static void waveform_read_complete(void *reference,IOReturn status,void *argument){
    struct waveform_slot *slot=reference;
    uint32_t completion_generation=slot?slot->generation:0;
    if(slot)atomic_store_explicit(&slot->in_flight,false,memory_order_release);
    if(!slot||!atomic_load_explicit(&waveform_drain_running,memory_order_acquire))return;
    if(completion_generation!=atomic_load_explicit(&waveform_generation,memory_order_acquire)){
        (void)submit_waveform_read(slot);
        return;
    }
    if(status==kIOReturnSuccess){
        uint64_t completed=atomic_fetch_add_explicit(&waveform_completions,1,memory_order_relaxed)+1;
        uint64_t now=monotonic_ns();
        uint64_t last_demand=shared_capture_last_demand_ns?atomic_load_explicit(shared_capture_last_demand_ns,memory_order_acquire):atomic_load_explicit(&capture_last_demand_ns,memory_order_acquire);
        bool requested=shared_capture_requested?atomic_load_explicit(shared_capture_requested,memory_order_acquire):atomic_load_explicit(&capture_active,memory_order_acquire);
        if(requested&&last_demand&&now-last_demand<250000000ull&&
           atomic_load_explicit(&waveform_drain_running,memory_order_acquire)&&
           completion_generation==atomic_load_explicit(&waveform_generation,memory_order_acquire)){
            UInt32 bytes=(UInt32)(uintptr_t)argument;
            float decoded[(WAVEFORM_READ_BYTES/NS6_CAPTURE_SOURCE_STRIDE)*NS6_CAPTURE_CHANNELS];
            size_t frames=ns6_capture_decode_block(slot->data,bytes,decoded,WAVEFORM_READ_BYTES/NS6_CAPTURE_SOURCE_STRIDE);
            bool still_current=atomic_load_explicit(&waveform_drain_running,memory_order_acquire)&&
                               completion_generation==atomic_load_explicit(&waveform_generation,memory_order_acquire);
            if(still_current){
                size_t accepted=ns6_capture_ring_write(capture_ring,decoded,frames);
                atomic_fetch_add_explicit(&capture_frames_decoded,frames,memory_order_relaxed);
                if(accepted<frames)atomic_fetch_add_explicit(&capture_ring_drops,frames-accepted,memory_order_relaxed);
            }
        }else if(last_demand&&now-last_demand>=250000000ull){
            atomic_store_explicit(&capture_active,false,memory_order_release);
            if(shared_capture_requested)atomic_store_explicit(shared_capture_requested,false,memory_order_release);
        }
        if(completed%10000==0)
            os_log(OS_LOG_DEFAULT,"Numark NS6 bulk-IN endpoint active: %llu reads, %llu errors, %llu capture frames decoded, %llu frames dropped",(unsigned long long)completed,(unsigned long long)atomic_load_explicit(&waveform_errors,memory_order_relaxed),(unsigned long long)atomic_load_explicit(&capture_frames_decoded,memory_order_relaxed),(unsigned long long)atomic_load_explicit(&capture_ring_drops,memory_order_relaxed));
    }else{
        uint64_t errors=atomic_fetch_add_explicit(&waveform_errors,1,memory_order_relaxed)+1;
        if(should_log_once_per_second(&waveform_last_log_ms))
            os_log_error(OS_LOG_DEFAULT,"Numark NS6 bulk-IN endpoint read failed: 0x%08x (%llu errors)",(unsigned)status,(unsigned long long)errors);
        if(status==kIOUSBPipeStalled&&atomic_load_explicit(&waveform_drain_running,memory_order_acquire)){
            IOReturn clear=(*auxiliary_interface)->ClearPipeStallBothEnds(auxiliary_interface,waveform_pipe);
            if(clear!=kIOReturnSuccess&&should_log_once_per_second(&waveform_last_log_ms))
                os_log_error(OS_LOG_DEFAULT,"Numark NS6 bulk-IN endpoint stall recovery failed: 0x%08x",(unsigned)clear);
        }
    }
    IOReturn result=submit_waveform_read(slot);
    if(result!=kIOReturnSuccess){
        uint64_t errors=atomic_fetch_add_explicit(&waveform_errors,1,memory_order_relaxed)+1;
        if(should_log_once_per_second(&waveform_last_log_ms))
            os_log_error(OS_LOG_DEFAULT,"Numark NS6 bulk-IN endpoint read resubmit failed: 0x%08x (%llu errors)",(unsigned)result,(unsigned long long)errors);
    }
}
static void start_waveform_drain(void){
    if(!waveform_pipe)return;
    atomic_fetch_add_explicit(&waveform_generation,1,memory_order_acq_rel);
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
static void stop_waveform_drain(void){
    bool was_running=atomic_exchange_explicit(&waveform_drain_running,false,memory_order_acq_rel);
    atomic_fetch_add_explicit(&waveform_generation,1,memory_order_acq_rel);
    atomic_store_explicit(&capture_active,false,memory_order_release);
    atomic_store_explicit(&capture_last_demand_ns,0,memory_order_release);
    if(shared_capture_requested)atomic_store_explicit(shared_capture_requested,false,memory_order_release);
    if(shared_capture_last_demand_ns)atomic_store_explicit(shared_capture_last_demand_ns,0,memory_order_release);
    if(was_running&&auxiliary_interface&&waveform_pipe)(*auxiliary_interface)->AbortPipe(auxiliary_interface,waveform_pipe);
    os_log(OS_LOG_DEFAULT,"Numark NS6 bulk-IN reads paused after capture demand ended");
}
void ns6_usb_reset_capture_session(void){
    atomic_store_explicit(&capture_active,false,memory_order_release);
    atomic_store_explicit(&capture_last_demand_ns,0,memory_order_release);
    if(shared_capture_requested)atomic_store_explicit(shared_capture_requested,false,memory_order_release);
    if(shared_capture_last_demand_ns)atomic_store_explicit(shared_capture_last_demand_ns,0,memory_order_release);
    atomic_fetch_add_explicit(&waveform_generation,1,memory_order_acq_rel);
}
static void feedback_submit(struct feedback_slot *slot);
static void feedback_complete(void *reference,IOReturn status,void *argument){
    (void)argument;
    struct feedback_slot *slot=reference;
    uint64_t callback_ticks=mach_absolute_time();
    uint64_t callback_ns=monotonic_ns();
    ns6_usb_timing_record(&feedback_completion_interval_timing,callback_ns);
    NS6USBFrameTimestampRange timestamp_range=ns6_usb_frame_timestamp_range(slot->frames,FEEDBACK_PACKETS);
    uint64_t dispatch_lag_ticks=0;
    if(usb_timebase.denom&&ns6_usb_frame_timestamp_callback_lag_ticks(timestamp_range,callback_ticks,&dispatch_lag_ticks))
        ns6_usb_timing_record_duration(&feedback_completion_dispatch_lag_timing,mach_ticks_to_ns(dispatch_lag_ticks));
    unsigned slot_index=(unsigned)(slot-feedback);
    if(slot_index<FEEDBACK_SLOT_COUNT)
        atomic_store_explicit(&feedback_in_flight[slot_index],false,memory_order_release);
    if(!atomic_load_explicit(&running,memory_order_acquire))return;
    if(status!=kIOReturnSuccess){
        uint64_t errors=atomic_fetch_add_explicit(&feedback_packet_errors,1,memory_order_relaxed)+1;
        uint64_t now_ms=monotonic_ms(),age_ms=0,last_valid=atomic_load_explicit(&feedback_last_valid_ms,memory_order_acquire);
        bool have_age=ns6_feedback_elapsed_ms(now_ms,last_valid,&age_ms);
        if(should_log_once_per_second(&feedback_transport_log_ms))
            os_log_error(OS_LOG_DEFAULT,"Numark NS6 feedback read failure at monotonic %llu ms: slot %u, status 0x%08x, valid-feedback age %llu ms (known %u), queue %u frames, cumulative errors %llu",(unsigned long long)now_ms,(unsigned)(slot-feedback),(unsigned)status,(unsigned long long)age_ms,have_age?1u:0u,ns6_transport_available(),(unsigned long long)errors);
        feedback_submit(slot);return;
    }
    for(unsigned p=0;p<FEEDBACK_PACKETS;++p){
        IOUSBLowLatencyIsocFrame *frame=&slot->frames[p];
        if(frame->frStatus!=kIOReturnSuccess){
            uint64_t errors=atomic_fetch_add_explicit(&feedback_packet_errors,1,memory_order_relaxed)+1;
            uint64_t now_ms=monotonic_ms(),age_ms=0,last_valid=atomic_load_explicit(&feedback_last_valid_ms,memory_order_acquire);
            bool have_age=ns6_feedback_elapsed_ms(now_ms,last_valid,&age_ms);
            if(should_log_once_per_second(&feedback_transport_log_ms))
                os_log_error(OS_LOG_DEFAULT,"Numark NS6 feedback frame error at monotonic %llu ms: slot %u packet %u, status 0x%08x, actual/requested %u/%u bytes, valid-feedback age %llu ms (known %u), queue %u frames, cumulative errors %llu",(unsigned long long)now_ms,(unsigned)(slot-feedback),p,(unsigned)frame->frStatus,(unsigned)frame->frActCount,(unsigned)frame->frReqCount,(unsigned long long)age_ms,have_age?1u:0u,ns6_transport_available(),(unsigned long long)errors);
            continue;
        }
        if(frame->frActCount<3){
            uint64_t total=atomic_fetch_add_explicit(&feedback_short_packets,1,memory_order_relaxed)+1;
            uint64_t now_ms=monotonic_ms(),age_ms=0,last_valid=atomic_load_explicit(&feedback_last_valid_ms,memory_order_acquire);
            bool have_age=ns6_feedback_elapsed_ms(now_ms,last_valid,&age_ms);
            if(should_log_once_per_second(&feedback_short_log_ms))
                os_log_error(OS_LOG_DEFAULT,"Numark NS6 short feedback packet at monotonic %llu ms: slot %u packet %u, actual/requested %u/%u bytes, status 0x%08x, valid-feedback age %llu ms (known %u), clock locked %u, queue %u frames, cumulative short packets %llu",(unsigned long long)now_ms,(unsigned)(slot-feedback),p,(unsigned)frame->frActCount,(unsigned)frame->frReqCount,(unsigned)frame->frStatus,(unsigned long long)age_ms,have_age?1u:0u,ns6_clock_control_is_locked(&clock_control)?1u:0u,ns6_transport_available(),(unsigned long long)total);
            continue;
        }
        const unsigned char *packet=slot->data+p*feedback_packet_bytes;
        feedback_last_sample[0]=packet[0];feedback_last_sample[1]=packet[1];feedback_last_sample[2]=packet[2];
        atomic_fetch_add_explicit(&feedback_samples,1,memory_order_relaxed);
        bool valid_clock_feedback=ns6_clock_control_feedback_value_is_valid(packet[0]);
        uint64_t now_ms=monotonic_ms();
        if(!valid_clock_feedback){
            uint64_t total=atomic_fetch_add_explicit(&feedback_out_of_range_values,1,memory_order_relaxed)+1;
            uint64_t age_ms=0,last_valid=atomic_load_explicit(&feedback_last_valid_ms,memory_order_acquire);
            bool have_age=ns6_feedback_elapsed_ms(now_ms,last_valid,&age_ms);
            if(should_log_once_per_second(&feedback_invalid_log_ms))
                os_log_error(OS_LOG_DEFAULT,"Numark NS6 out-of-range feedback at monotonic %llu ms: slot %u packet %u, value %u (bytes %02x %02x %02x), actual/requested %u/%u, valid range 42..46, valid-feedback age %llu ms (known %u), clock locked %u, queue %u frames, cumulative invalid values %llu",(unsigned long long)now_ms,(unsigned)(slot-feedback),p,(unsigned)packet[0],packet[0],packet[1],packet[2],(unsigned)frame->frActCount,(unsigned)frame->frReqCount,(unsigned long long)age_ms,have_age?1u:0u,ns6_clock_control_is_locked(&clock_control)?1u:0u,ns6_transport_available(),(unsigned long long)total);
        }
        bool was_locked=ns6_clock_control_is_locked(&clock_control);
        if(packet[0]==44){atomic_fetch_add_explicit(&feedback_44_frames,1,memory_order_relaxed);}
        else if(packet[0]==45){atomic_fetch_add_explicit(&feedback_45_frames,1,memory_order_relaxed);}
        else atomic_fetch_add_explicit(&feedback_other_values,1,memory_order_relaxed);
        if(valid_clock_feedback)ns6_clock_control_observe(&clock_control,packet[0]);
        if(valid_clock_feedback)
            atomic_store_explicit(&feedback_last_valid_ms,now_ms,memory_order_release);
        if(!was_locked&&ns6_clock_control_is_locked(&clock_control)){
            atomic_fetch_add_explicit(&feedback_lock_events,1,memory_order_relaxed);
            uint64_t stale_started=atomic_exchange_explicit(&feedback_stale_reset_started_ms,0,memory_order_acq_rel);
            uint64_t recovery_ms=0;
            bool recovering=ns6_feedback_elapsed_ms(now_ms,stale_started,&recovery_ms);
            if(recovering)
                os_log(OS_LOG_DEFAULT,"Numark NS6 feedback lock reacquired at monotonic %llu ms after %llu ms; first frame value %u; queue %u frames; rate %.3f Hz",(unsigned long long)now_ms,(unsigned long long)recovery_ms,(unsigned)packet[0],ns6_transport_available(),(double)ns6_clock_control_rate_millihz(&clock_control)/1000.0);
            else
                os_log(OS_LOG_DEFAULT,"Numark NS6 USB feedback active at monotonic %llu ms: first frame value %u; queue %u frames; %.3f Hz diagnostic; clock mode %{public}s",(unsigned long long)now_ms,(unsigned)packet[0],ns6_transport_available(),(double)ns6_clock_control_rate_millihz(&clock_control)/1000.0,ns6_clock_control_is_adaptive_enabled()?"adaptive":"fixed nominal (diagnostic only)");
        }
    }
    if(should_log_once_per_second(&feedback_last_log_ms)){
        uint64_t samples=atomic_load_explicit(&feedback_samples,memory_order_relaxed);
        uint64_t count44=atomic_load_explicit(&feedback_44_frames,memory_order_relaxed);
        uint64_t count45=atomic_load_explicit(&feedback_45_frames,memory_order_relaxed);
        uint64_t other=atomic_load_explicit(&feedback_other_values,memory_order_relaxed);
        uint64_t window44=count44-feedback_logged_44, window45=count45-feedback_logged_45;
        uint64_t window_other=other-feedback_logged_other, window_samples=samples-feedback_logged_samples;
        uint64_t short_packets=atomic_load_explicit(&feedback_short_packets,memory_order_relaxed);
        uint64_t out_of_range=atomic_load_explicit(&feedback_out_of_range_values,memory_order_relaxed);
        uint64_t lock_events=atomic_load_explicit(&feedback_lock_events,memory_order_relaxed);
        uint64_t stale_resets=atomic_load_explicit(&feedback_stale_resets,memory_order_relaxed);
        uint64_t valid=window44+window45;
        double average=valid?44.0+(double)window45/(double)valid:0.0;
        os_log(OS_LOG_DEFAULT,"Numark NS6 USB clock feedback: %llu samples, 44=%llu, 45=%llu, other=%llu, mean %.4f frames/report; short packets +%llu total %llu; out-of-range +%llu total %llu; lock events +%llu total %llu; stale-feedback resets +%llu total %llu; mode %{public}s; diagnostic feedback %.3f Hz; output rate %.3f Hz; source step %.8f; request total %llu, sent %llu, debt %lld; queue %u frames; cumulative %llu samples; last %02x %02x %02x; USB read/submit/frame errors %llu",(unsigned long long)window_samples,(unsigned long long)window44,(unsigned long long)window45,(unsigned long long)window_other,average,(unsigned long long)(short_packets-feedback_logged_short),(unsigned long long)short_packets,(unsigned long long)(out_of_range-feedback_logged_out_of_range),(unsigned long long)out_of_range,(unsigned long long)(lock_events-feedback_logged_lock_events),(unsigned long long)lock_events,(unsigned long long)(stale_resets-feedback_logged_stale_resets),(unsigned long long)stale_resets,ns6_clock_control_is_adaptive_enabled()?(ns6_clock_control_is_locked(&clock_control)?"adaptive":"adaptive warmup"):"fixed nominal (diagnostic)",(double)ns6_clock_control_rate_millihz(&clock_control)/1000.0,(double)ns6_clock_control_output_rate_millihz(&clock_control)/1000.0,ns6_clock_control_source_frames_per_output_frame(&clock_control,ns6_transport_available()),(unsigned long long)ns6_clock_control_requested_total(&clock_control),(unsigned long long)ns6_clock_control_sent_total(&clock_control),(long long)ns6_clock_control_debt(&clock_control),ns6_transport_available(),(unsigned long long)samples,feedback_last_sample[0],feedback_last_sample[1],feedback_last_sample[2],(unsigned long long)atomic_load_explicit(&feedback_packet_errors,memory_order_relaxed));
        feedback_logged_samples=samples;feedback_logged_44=count44;feedback_logged_45=count45;feedback_logged_other=other;
        feedback_logged_short=short_packets;feedback_logged_out_of_range=out_of_range;
        feedback_logged_lock_events=lock_events;feedback_logged_stale_resets=stale_resets;
    }
    feedback_submit(slot);
}
static void feedback_submit(struct feedback_slot *slot){
    if(!slot||!feedback_pipe||!slot->data||!slot->frames)return;
    unsigned slot_index=(unsigned)(slot-feedback);
    if(slot_index>=FEEDBACK_SLOT_COUNT)return;
    bool running_now=atomic_load_explicit(&running,memory_order_acquire);
    bool in_flight=atomic_load_explicit(&feedback_in_flight[slot_index],memory_order_acquire);
    if(!ns6_feedback_read_should_submit(running_now,in_flight))return;
    bool expected=false;
    if(!atomic_compare_exchange_strong_explicit(&feedback_in_flight[slot_index],&expected,true,memory_order_acq_rel,memory_order_acquire))return;
    if(!atomic_load_explicit(&running,memory_order_acquire)){
        atomic_store_explicit(&feedback_in_flight[slot_index],false,memory_order_release);
        return;
    }
    for(unsigned p=0;p<FEEDBACK_PACKETS;++p){slot->frames[p].frReqCount=feedback_packet_bytes;slot->frames[p].frActCount=0;slot->frames[p].frStatus=0;}
    IOReturn result=(*auxiliary_interface)->LowLatencyReadIsochPipeAsync(auxiliary_interface,feedback_pipe,slot->data,0,FEEDBACK_PACKETS,1,slot->frames,feedback_complete,slot);
    if(result!=kIOReturnSuccess){
        atomic_store_explicit(&feedback_in_flight[slot_index],false,memory_order_release);
        uint64_t errors=atomic_fetch_add_explicit(&feedback_packet_errors,1,memory_order_relaxed)+1;
        uint64_t now_ms=monotonic_ms(),age_ms=0,last_valid=atomic_load_explicit(&feedback_last_valid_ms,memory_order_acquire);
        bool have_age=ns6_feedback_elapsed_ms(now_ms,last_valid,&age_ms);
        if(should_log_once_per_second(&feedback_transport_log_ms))
            os_log_error(OS_LOG_DEFAULT,"Numark NS6 feedback submit failure at monotonic %llu ms: slot %u, status 0x%08x, valid-feedback age %llu ms (known %u), queue %u frames, cumulative errors %llu",(unsigned long long)now_ms,(unsigned)(slot-feedback),(unsigned)result,(unsigned long long)age_ms,have_age?1u:0u,ns6_transport_available(),(unsigned long long)errors);
    }
}
static bool usb_failure(const char *operation,IOReturn result){fprintf(stderr,"Numark NS6 %s failed: 0x%08x\n",operation,result);os_log_error(OS_LOG_DEFAULT,"Numark NS6 %{public}s failed: 0x%08x",operation,(unsigned)result);return false;}
static IOReturn control(UInt8 type,UInt8 request,UInt16 value,UInt16 index,void *data,UInt16 length){
    IOUSBDevRequest r={type,request,value,index,length,data,0}; return (*interface)->ControlRequest(interface,0,&r);
}
static void read_firmware_version(void){
    uint8_t response[8]={0};char version[32]={0};
    atomic_store_explicit(&firmware_response_valid,false,memory_order_release);
    IOUSBDevRequest request={0xc0,0x56,0,0,sizeof(response),response,0};
    IOReturn result=(*interface)->ControlRequest(interface,0,&request);
    if(result!=kIOReturnSuccess||request.wLenDone<3||request.wLenDone>sizeof(response)){
        os_log_error(OS_LOG_DEFAULT,"Numark NS6 firmware query failed: status 0x%08x, returned %u of %zu bytes",(unsigned)result,request.wLenDone,sizeof(response));
        return;
    }
    os_log(OS_LOG_DEFAULT,"Numark NS6 firmware raw response (%u bytes): %02x %02x %02x %02x %02x %02x %02x %02x",request.wLenDone,response[0],response[1],response[2],response[3],response[4],response[5],response[6],response[7]);
    if(!ns6_firmware_version_decode(response,request.wLenDone,version)){
        os_log_error(OS_LOG_DEFAULT,"Numark NS6 firmware response has an unsupported format");
        return;
    }
    uint32_t packed=(uint32_t)response[0]|((uint32_t)response[1]<<8)|((uint32_t)response[2]<<16);
    atomic_store_explicit(&firmware_response_bits,packed,memory_order_relaxed);
    atomic_store_explicit(&firmware_response_valid,true,memory_order_release);
    os_log(OS_LOG_DEFAULT,"Numark NS6 firmware version response: %{public}s",version);
}
bool ns6_usb_get_firmware_version(char output[32]){
    if(!output)return false;
    output[0]=0;
    if(!atomic_load_explicit(&firmware_response_valid,memory_order_acquire))return false;
    uint32_t packed=atomic_load_explicit(&firmware_response_bits,memory_order_relaxed);
    uint8_t response[8]={(uint8_t)packed,(uint8_t)(packed>>8),(uint8_t)(packed>>16),0,0,0,0,0};
    return ns6_firmware_version_decode(response,sizeof(response),output);
}
static void fill(struct slot *slot){
    unsigned char *output=slot->data;
    unsigned millisecond_frames[8];
    for(unsigned packet=0;packet<PACKETS;++packet){
        uint64_t last_feedback=atomic_load_explicit(&feedback_last_valid_ms,memory_order_acquire);
        if(ns6_clock_control_is_locked(&clock_control)&&last_feedback){
            uint64_t now_ms=monotonic_ms(),feedback_age_ms=0;
            if(ns6_feedback_elapsed_ms(now_ms,last_feedback,&feedback_age_ms)&&feedback_age_ms>2000){
                ns6_clock_control_reset_feedback(&clock_control);
                atomic_fetch_add_explicit(&feedback_stale_resets,1,memory_order_relaxed);
                atomic_store_explicit(&feedback_stale_reset_started_ms,now_ms,memory_order_release);
                atomic_store_explicit(&feedback_last_valid_ms,0,memory_order_release);
                os_log_error(OS_LOG_DEFAULT,"Numark NS6 stale feedback reset at monotonic %llu ms: last valid report age %llu ms, queue %u frames, retaining nominal 44.1 kHz cadence",(unsigned long long)now_ms,(unsigned long long)feedback_age_ms,ns6_transport_available());
            }
        }
        if((packet % 8u) == 0)
            ns6_clock_control_next_millisecond(&clock_control, millisecond_frames);
        unsigned frames=millisecond_frames[packet % 8u];
        slot->frames[packet].frReqCount=(UInt16)(frames*FRAME_BYTES); slot->frames[packet].frActCount=0; slot->frames[packet].frStatus=0;
        if(atomic_load_explicit(&output_paused,memory_order_acquire)){
            ns6_transport_discard();
            have_last_recovery_sample=false;
            /* Some NS6 units keep the USB indicator in its searching state
               until the first non-zero audio sample arrives. Prime one USB
               service interval at the smallest 24-bit PCM value; this is
               far below the DAC's analog noise floor and is not an audible
               tone. */
            bool keep_alive=false;
#if NS6_IDLE_PCM_KEEPALIVE
            keep_alive=true;
#else
            keep_alive=frames&&atomic_exchange_explicit(&standby_prime_pending,false,memory_order_acq_rel);
#endif
            ns6_fill_idle_pcm(output,frames,keep_alive);
            output+=frames*FRAME_BYTES;
            continue;
        }
        if(atomic_load_explicit(&awaiting_audio_prefill,memory_order_acquire)){
            UInt32 startup_frames=ns6_audio_startup_prefill_frames(
                atomic_load_explicit(&startup_buffer_frames,memory_order_acquire),
                NS6_STARTUP_MIN_PREFILL_FRAMES,
                NS6_CLOCK_QUEUE_TARGET_FRAMES);
            if(ns6_transport_available()<startup_frames){
                have_last_recovery_sample=false;
                memset(output,0,frames*FRAME_BYTES);
                output+=frames*FRAME_BYTES;
                continue;
            }
            atomic_store_explicit(&awaiting_audio_prefill,false,memory_order_release);
            have_last_recovery_sample=false;
        }
        UInt32 available=ns6_transport_available();
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
        if(received){
            const unsigned char *last=output+(received-1)*FRAME_BYTES;
            for(unsigned channel=0;channel<4;++channel)
                last_recovery_sample[channel]=unpack_pcm24(last+channel*3);
            have_last_recovery_sample=true;
        }
        if(received<frames&&have_last_recovery_sample){
            unsigned missing=frames-received;
            for(unsigned frame=0;frame<missing;++frame){
                double gain=ns6_audio_recovery_underrun_gain(frame,missing);
                for(unsigned channel=0;channel<4;++channel)
                    pack_pcm24(output+(received+frame)*FRAME_BYTES+channel*3,
                               (int32_t)((double)last_recovery_sample[channel]*gain));
            }
            have_last_recovery_sample=false;
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
    uint64_t callback_ticks=mach_absolute_time();
    slot->callback_ns=monotonic_ns();
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
    UInt32 packets_60=0,packets_72=0,packets_other_size=0;
    IOReturn packet_error_status=kIOReturnSuccess;
    for(unsigned p=0;p<PACKETS;++p){
        IOUSBLowLatencyIsocFrame *frame=&slot->frames[p];
        if(frame->frReqCount==60)++packets_60;
        else if(frame->frReqCount==72)++packets_72;
        else ++packets_other_size;
        if(frame->frStatus!=kIOReturnSuccess){
            ++packet_errors;
            packet_error_status=frame->frStatus;
        }else if(frame->frActCount<frame->frReqCount){
            ++short_packets;
            short_bytes+=(UInt64)(frame->frReqCount-frame->frActCount);
        }
    }
    atomic_fetch_add_explicit(&isoc_60_byte_packets,packets_60,memory_order_relaxed);
    atomic_fetch_add_explicit(&isoc_72_byte_packets,packets_72,memory_order_relaxed);
    atomic_fetch_add_explicit(&isoc_other_size_packets,packets_other_size,memory_order_relaxed);
    NS6USBFrameTimestampRange timestamp_range=ns6_usb_frame_timestamp_range(slot->frames,PACKETS);
    uint64_t dispatch_lag_ticks=0;
    if(usb_timebase.denom&&ns6_usb_frame_timestamp_callback_lag_ticks(timestamp_range,callback_ticks,&dispatch_lag_ticks))
        ns6_usb_timing_record_duration(&completion_dispatch_lag_timing,mach_ticks_to_ns(dispatch_lag_ticks));
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
        os_log(OS_LOG_DEFAULT,"Numark NS6 audio health: transfers %llu, underruns %llu events/%llu frames (max burst %llu), USB transfer errors %llu, packet errors %llu (last 0x%08x), short packets %llu/%llu bytes, OUT requested packet sizes 60/72/other %llu/%llu/%llu, queue %u frames",(unsigned long long)completed,(unsigned long long)atomic_load_explicit(&underrun_events,memory_order_relaxed),(unsigned long long)atomic_load_explicit(&underrun_frames,memory_order_relaxed),(unsigned long long)atomic_load_explicit(&underrun_max_burst,memory_order_relaxed),(unsigned long long)atomic_load_explicit(&isoc_transfer_errors,memory_order_relaxed),(unsigned long long)atomic_load_explicit(&isoc_packet_errors,memory_order_relaxed),(unsigned)atomic_load_explicit(&last_packet_status,memory_order_relaxed),(unsigned long long)atomic_load_explicit(&isoc_short_packets,memory_order_relaxed),(unsigned long long)atomic_load_explicit(&isoc_short_bytes,memory_order_relaxed),(unsigned long long)atomic_exchange_explicit(&isoc_60_byte_packets,0,memory_order_relaxed),(unsigned long long)atomic_exchange_explicit(&isoc_72_byte_packets,0,memory_order_relaxed),(unsigned long long)atomic_exchange_explicit(&isoc_other_size_packets,0,memory_order_relaxed),ns6_transport_available());
        NS6IOMetricsSnapshot io_snapshots[NS6_IO_METRICS_MAX_CLIENTS];
        size_t io_count=ns6_io_metrics_take_snapshot(&ns6_io_metrics,io_snapshots,NS6_IO_METRICS_MAX_CLIENTS);
        for(size_t n=0;n<io_count;++n){
            const NS6IOMetricsSnapshot *io=&io_snapshots[n];
            double mean_frames=io->calls?(double)io->frames/(double)io->calls:0.0;
            double mean_interval_us=io->interval_count?(double)io->interval_total_ns/(double)io->interval_count/1000.0:0.0;
            os_log(OS_LOG_DEFAULT,"Numark NS6 CoreAudio WriteMix: client %u, %llu calls, mean %.1f frames/cycle, mean gap %.1f us, max gap %llu us, max enqueue %llu us, enqueue-over-period %llu",io->client_id,(unsigned long long)io->calls,mean_frames,mean_interval_us,(unsigned long long)(io->max_interval_ns/1000),(unsigned long long)(io->max_callback_ns/1000),(unsigned long long)io->deadline_misses);
        }
        NS6USBTimingSnapshot cadence={0},dispatch_lag={0},pre_fill={0},fill_work={0},fill_cpu={0},work={0},submit_api={0},submit_api_cpu={0};
        if(ns6_usb_timing_take_snapshot(&completion_interval_timing,&cadence))
            os_log(OS_LOG_DEFAULT,"Numark NS6 USB cadence: %llu intervals, mean %.1f us, min %llu us, max %llu us",(unsigned long long)cadence.intervals,(double)cadence.total_ns/(double)cadence.intervals/1000.0,(unsigned long long)(cadence.min_ns/1000),(unsigned long long)(cadence.max_ns/1000));
        if(ns6_usb_timing_take_snapshot(&completion_dispatch_lag_timing,&dispatch_lag))
            os_log(OS_LOG_DEFAULT,"Numark NS6 USB frame-to-callback dispatch lag: %llu samples, mean %.1f us, min %llu us, max %llu us",(unsigned long long)dispatch_lag.intervals,(double)dispatch_lag.total_ns/(double)dispatch_lag.intervals/1000.0,(unsigned long long)(dispatch_lag.min_ns/1000),(unsigned long long)(dispatch_lag.max_ns/1000));
        NS6USBTimingSnapshot feedback_cadence={0},feedback_dispatch_lag={0};
        if(ns6_usb_timing_take_snapshot(&feedback_completion_interval_timing,&feedback_cadence))
            os_log(OS_LOG_DEFAULT,"Numark NS6 USB feedback callback cadence: %llu intervals, mean %.1f us, min %llu us, max %llu us, paused %u",(unsigned long long)feedback_cadence.intervals,(double)feedback_cadence.total_ns/(double)feedback_cadence.intervals/1000.0,(unsigned long long)(feedback_cadence.min_ns/1000),(unsigned long long)(feedback_cadence.max_ns/1000),atomic_load_explicit(&output_paused,memory_order_acquire)?1u:0u);
        if(ns6_usb_timing_take_snapshot(&feedback_completion_dispatch_lag_timing,&feedback_dispatch_lag))
            os_log(OS_LOG_DEFAULT,"Numark NS6 USB feedback frame-to-callback dispatch lag: %llu samples, mean %.1f us, min %llu us, max %llu us, paused %u",(unsigned long long)feedback_dispatch_lag.intervals,(double)feedback_dispatch_lag.total_ns/(double)feedback_dispatch_lag.intervals/1000.0,(unsigned long long)(feedback_dispatch_lag.min_ns/1000),(unsigned long long)(feedback_dispatch_lag.max_ns/1000),atomic_load_explicit(&output_paused,memory_order_acquire)?1u:0u);
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
    IOReturn result=(*interface)->USBInterfaceOpenSeize(interface); if(result!=kIOReturnSuccess){log_ns6_interface_owner(0,result);return usb_failure("open interface",result);}
    auxiliary_interface=open_interface_once(1); if(!auxiliary_interface){os_log_error(OS_LOG_DEFAULT,"Numark NS6 could not find USB interface 1");return false;}
    result=(*auxiliary_interface)->USBInterfaceOpenSeize(auxiliary_interface); if(result!=kIOReturnSuccess){log_ns6_interface_owner(1,result);return usb_failure("open auxiliary interface",result);}
    result=(*auxiliary_interface)->SetAlternateInterface(auxiliary_interface,1); if(result!=kIOReturnSuccess)return usb_failure("select auxiliary alternate setting",result);
    (void)prepare_waveform_drain();
    result=(*interface)->CreateInterfaceAsyncEventSource(interface,&source); if(result!=kIOReturnSuccess||!source)return usb_failure("create async source",result);
    run_loop=CFRunLoopGetCurrent(); CFRetain(run_loop); CFRunLoopAddSource(run_loop,source,kCFRunLoopDefaultMode);
    result=(*auxiliary_interface)->CreateInterfaceAsyncEventSource(auxiliary_interface,&feedback_source); if(result!=kIOReturnSuccess||!feedback_source)return usb_failure("create auxiliary async source",result);
    CFRunLoopAddSource(run_loop,feedback_source,kCFRunLoopDefaultMode);
    result=(*interface)->SetAlternateInterface(interface,1); if(result!=kIOReturnSuccess)return usb_failure("select alternate setting",result);
    result=control(0xc0,86,0,0,capability,8); if(result!=kIOReturnSuccess)return usb_failure("read capability",result);
    UInt16 capability_length=capability[0]<sizeof(capability)?capability[0]:(UInt16)sizeof(capability);
    if(capability_length){result=control(0xc0,86,0,0,capability,capability_length);if(result!=kIOReturnSuccess)return usb_failure("read capability details",result);}
    read_firmware_version();
    result=control(0x22,1,0x0100,134,rate,sizeof(rate));if(result!=kIOReturnSuccess)return usb_failure("set clock selector 134",result);
    result=control(0x22,1,0x0100,2,rate,sizeof(rate));if(result!=kIOReturnSuccess)return usb_failure("set clock selector 2",result);
    UInt8 input_mode=0;
    result=control(0xc0,73,0,0,&input_mode,sizeof(input_mode));if(result!=kIOReturnSuccess)return usb_failure("read audio input mode",result);
    UInt16 selected_input_mode=(UInt16)((input_mode&~0x20u)|0x30u);
    result=control(0x40,73,selected_input_mode,0,NULL,0);if(result!=kIOReturnSuccess)return usb_failure("set audio input mode",result);
    os_log(OS_LOG_DEFAULT,"Numark NS6 audio input mode: device 0x%02x -> host-selected two-channel mode 0x%02x",input_mode,selected_input_mode);
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
    for(unsigned i=0;i<FEEDBACK_SLOT_COUNT;++i)atomic_store_explicit(&feedback_in_flight[i],false,memory_order_release);
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
        os_log(OS_LOG_DEFAULT,"Numark NS6 diagnostic USB OUT queue: %u transfers x %u microframes (%u queued microframes); feedback polling %{public}s; Hopper frame-pattern scheduler %{public}s",(unsigned)SLOT_COUNT,(unsigned)PACKETS,(unsigned)(SLOT_COUNT*PACKETS),ns6_usb_feedback_reader_enabled()?"enabled":"disabled",ns6_clock_control_is_feedback_pattern_enabled()?"enabled":"disabled");
        for(unsigned i=0;i<FEEDBACK_SLOT_COUNT;++i)feedback_submit(&feedback[i]);
        bool midi_session_active=ns6_midi_start(interface,run_loop);
        if(midi_session_active){
            (void)ns6_midi_handshake(run_loop);
#if NS6_KEEP_WAVEFORM_DRAIN_IDLE
            os_log(OS_LOG_DEFAULT,"Numark NS6 diagnostic: keeping endpoint 0x86 bulk-IN reads active while idle");
#else
            if(ns6_usb_midi_input_enabled())
                os_log(OS_LOG_DEFAULT,"Numark NS6 waveform endpoint drain disabled during active MIDI session");
            else
                os_log(OS_LOG_DEFAULT,"Numark NS6 diagnostic audio-only run completed outbound activation; input polling and waveform drain disabled");
#endif
        }else{
            fprintf(stderr,"Numark NS6 MIDI unavailable; continuing with audio only\n");
            if(ns6_should_run_waveform_drain(midi_session_active)){
                os_log(OS_LOG_DEFAULT,"Numark NS6 MIDI unavailable; enabling waveform endpoint drain for audio-only session");
                start_waveform_drain();
            }
        }
        bool feedback_reads_active=false;
        while(atomic_load_explicit(&running,memory_order_acquire)&&!atomic_load_explicit(&recovery_requested,memory_order_acquire)){
            CFRunLoopRunInMode(kCFRunLoopDefaultMode,0.1,false);
            uint64_t capture_demand=shared_capture_last_demand_ns?atomic_load_explicit(shared_capture_last_demand_ns,memory_order_acquire):atomic_load_explicit(&capture_last_demand_ns,memory_order_acquire);
            bool capture_active_now=shared_capture_requested?atomic_load_explicit(shared_capture_requested,memory_order_acquire):atomic_load_explicit(&capture_active,memory_order_acquire);
            bool capture_requested=capture_active_now&&capture_demand&&monotonic_ns()-capture_demand<250000000ull;
            bool should_run_bulk_in=ns6_bulk_in_reads_should_run(midi_session_active,capture_requested);
            bool bulk_in_running=atomic_load_explicit(&waveform_drain_running,memory_order_acquire);
            if(capture_requested&&!bulk_in_running){
                os_log(OS_LOG_DEFAULT,"Numark NS6 CoreAudio requested capture; starting endpoint 0x86 bulk reads while MIDI session is active");
                start_waveform_drain();
            }else if(!should_run_bulk_in&&bulk_in_running){
                stop_waveform_drain();
            }else if(should_run_bulk_in&&!bulk_in_running){
                if(midi_session_active&&!capture_requested)
                    os_log(OS_LOG_DEFAULT,"Numark NS6 diagnostic: starting persistent endpoint 0x86 reads with no capture client");
                start_waveform_drain();
            }
            bool should_read_feedback=atomic_load_explicit(&running,memory_order_acquire);
            if(should_read_feedback!=feedback_reads_active){
                feedback_reads_active=should_read_feedback;
                os_log(OS_LOG_DEFAULT,"Numark NS6 clock feedback reads %{public}s while USB transport is active",feedback_reads_active?"active":"inactive");
            }
            if(should_read_feedback)
                for(unsigned i=0;i<FEEDBACK_SLOT_COUNT;++i)feedback_submit(&feedback[i]);
        }
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
    if(!shared_capture_ring)ns6_capture_ring_init(capture_ring);atomic_store_explicit(&capture_active,false,memory_order_release);atomic_store_explicit(&capture_last_demand_ns,0,memory_order_release);atomic_store_explicit(&capture_frames_decoded,0,memory_order_relaxed);atomic_store_explicit(&capture_ring_drops,0,memory_order_relaxed);atomic_store_explicit(&waveform_drain_running,false,memory_order_release);
    mach_timebase_info(&usb_timebase); ns6_clock_control_init(&clock_control); ns6_usb_timing_init(&completion_interval_timing); ns6_usb_timing_init(&completion_dispatch_lag_timing); ns6_usb_timing_init(&feedback_completion_interval_timing); ns6_usb_timing_init(&feedback_completion_dispatch_lag_timing); ns6_usb_timing_init(&callback_pre_fill_timing); ns6_usb_timing_init(&fill_duration_timing); ns6_usb_timing_init(&fill_cpu_timing); ns6_usb_timing_init(&callback_submit_timing); ns6_usb_timing_init(&submit_api_timing); ns6_usb_timing_init(&submit_api_cpu_timing); initialized=false; atomic_store(&completed_transfers,0); atomic_store(&underrun_frames,0); atomic_store(&underrun_events,0); atomic_store(&underrun_max_burst,0); atomic_store(&isoc_packet_errors,0); atomic_store(&isoc_short_packets,0); atomic_store(&isoc_short_bytes,0); atomic_store(&isoc_60_byte_packets,0); atomic_store(&isoc_72_byte_packets,0); atomic_store(&isoc_other_size_packets,0); atomic_store(&isoc_transfer_errors,0); atomic_store(&last_underrun_log_ms,0); atomic_store(&last_packet_log_ms,0); atomic_store(&last_packet_status,kIOReturnSuccess); atomic_store(&feedback_samples,0); atomic_store(&feedback_44_frames,0); atomic_store(&feedback_45_frames,0); atomic_store(&feedback_other_values,0); atomic_store(&feedback_short_packets,0); atomic_store(&feedback_out_of_range_values,0); atomic_store(&feedback_lock_events,0); atomic_store(&feedback_stale_resets,0); atomic_store(&feedback_packet_errors,0); atomic_store(&feedback_last_log_ms,0); atomic_store(&feedback_short_log_ms,0); atomic_store(&feedback_invalid_log_ms,0); atomic_store(&feedback_transport_log_ms,0); atomic_store(&feedback_last_valid_ms,0); atomic_store(&feedback_stale_reset_started_ms,0); for(unsigned i=0;i<FEEDBACK_SLOT_COUNT;++i)atomic_store(&feedback_in_flight[i],false); feedback_logged_samples=feedback_logged_44=feedback_logged_45=feedback_logged_other=0; feedback_logged_short=feedback_logged_out_of_range=0; feedback_logged_lock_events=feedback_logged_stale_resets=0; memset(feedback_last_sample,0,sizeof(feedback_last_sample)); ns6_transport_reset(); atomic_store(&output_paused,true); atomic_store(&standby_prime_pending,true); atomic_store(&recovery_requested,false); atomic_store(&running,true);
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
void ns6_usb_log_io_transition(const char *event,uint32_t client,unsigned clients_before,unsigned clients_after){
    os_log(OS_LOG_DEFAULT,"Numark NS6 I/O boundary %{public}s: client %u, clients %u->%u, USB running %{public}s, output paused %{public}s, queue %u frames, OUT transfers %llu, underruns %llu events/%llu frames, USB errors %llu, packet errors %llu, short packets %llu/%llu bytes, OUT sizes since health 60/72/other %llu/%llu/%llu, feedback samples %llu, feedback errors %llu, endpoint 0x86 drain %{public}s",
        event?event:"unknown",client,clients_before,clients_after,
        atomic_load_explicit(&running,memory_order_acquire)?"yes":"no",
        atomic_load_explicit(&output_paused,memory_order_acquire)?"yes":"no",
        ns6_transport_available(),
        (unsigned long long)atomic_load_explicit(&completed_transfers,memory_order_relaxed),
        (unsigned long long)atomic_load_explicit(&underrun_events,memory_order_relaxed),
        (unsigned long long)atomic_load_explicit(&underrun_frames,memory_order_relaxed),
        (unsigned long long)atomic_load_explicit(&isoc_transfer_errors,memory_order_relaxed),
        (unsigned long long)atomic_load_explicit(&isoc_packet_errors,memory_order_relaxed),
        (unsigned long long)atomic_load_explicit(&isoc_short_packets,memory_order_relaxed),
        (unsigned long long)atomic_load_explicit(&isoc_short_bytes,memory_order_relaxed),
        (unsigned long long)atomic_load_explicit(&isoc_60_byte_packets,memory_order_relaxed),
        (unsigned long long)atomic_load_explicit(&isoc_72_byte_packets,memory_order_relaxed),
        (unsigned long long)atomic_load_explicit(&isoc_other_size_packets,memory_order_relaxed),
        (unsigned long long)atomic_load_explicit(&feedback_samples,memory_order_relaxed),
        (unsigned long long)atomic_load_explicit(&feedback_packet_errors,memory_order_relaxed),
        atomic_load_explicit(&waveform_drain_running,memory_order_acquire)?"active":"stopped");
}
size_t ns6_usb_capture_read(float *output,size_t frames){
    if(!output||frames==0)return 0;
    atomic_store_explicit(&capture_last_demand_ns,monotonic_ns(),memory_order_release);
    if(!atomic_exchange_explicit(&capture_active,true,memory_order_acq_rel))ns6_capture_ring_clear(capture_ring);
    return ns6_capture_ring_read(capture_ring,output,frames);
}
void ns6_usb_bind_shared_capture(NS6CaptureRing *ring,atomic_bool *requested,
                                atomic_uint_fast64_t *last_demand_ns){
    if(ring&&requested&&last_demand_ns){
        capture_ring=ring;shared_capture_ring=true;
        shared_capture_requested=requested;shared_capture_last_demand_ns=last_demand_ns;
    }else{
        capture_ring=&local_capture_ring;shared_capture_ring=false;
        shared_capture_requested=NULL;shared_capture_last_demand_ns=NULL;
    }
}
void ns6_usb_set_startup_buffer_frames(uint32_t frames){atomic_store_explicit(&startup_buffer_frames,frames,memory_order_release);}
void ns6_usb_submit_pcm(const uint8_t *pcm24,uint32_t frames){(void)pcm24;(void)frames;}
