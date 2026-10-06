#include "NS6Transport.h"
#include "NS6USB.h"
#include "NS6BufferFrameSize.h"
#include "NS6ZeroTimestamp.h"
#include <CoreAudio/AudioHardware.h>
#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreFoundation/CFPlugInCOM.h>
#include <CoreAudio/CoreAudio.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/usb/IOUSBLib.h>
#include <mach/mach_time.h>
#include <math.h>
#include <os/log.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { DEVICE_ID=2, STREAM_ID=3 };
#define SAMPLE_RATE 44100.0
#define CHANNELS 4
#define NS6_DRIVER_BUFFER_PROPERTY ((AudioObjectPropertySelector)0x6e733662u) /* 'ns6b' */
#define NS6_BUFFER_RESTART_PROPERTY ((AudioObjectPropertySelector)0x6e733672u) /* 'ns6r' */
#define NS6_ACTIVE_IO_FRAMES_PROPERTY ((AudioObjectPropertySelector)0x6e733666u) /* 'ns6f' */
#define NS6_ACTIVE_CLIENT_PROPERTY ((AudioObjectPropertySelector)0x6e733663u) /* 'ns6c' */
#define NS6_DRIVER_BUFFER_STORAGE_KEY CFSTR("driver-startup-buffer-frames")
#define NS6_TRACKED_CLIENTS 32
typedef struct { UInt32 client_id; pid_t pid; char bundle_id[256]; unsigned starts; UInt64 order; bool used; } NS6TrackedClient;
static NS6TrackedClient tracked_clients[NS6_TRACKED_CLIENTS];
static UInt64 client_order;
static const UInt32 supported_buffer_frames[]={49,128,192,256,512,1024};
static const AudioServerPlugInCustomPropertyInfo custom_property_info[]={{NS6_DRIVER_BUFFER_PROPERTY,kAudioServerPlugInCustomPropertyDataTypeCFString,kAudioServerPlugInCustomPropertyDataTypeNone},{NS6_BUFFER_RESTART_PROPERTY,kAudioServerPlugInCustomPropertyDataTypeCFString,kAudioServerPlugInCustomPropertyDataTypeNone},{NS6_ACTIVE_IO_FRAMES_PROPERTY,kAudioServerPlugInCustomPropertyDataTypeCFString,kAudioServerPlugInCustomPropertyDataTypeNone},{NS6_ACTIVE_CLIENT_PROPERTY,kAudioServerPlugInCustomPropertyDataTypeCFString,kAudioServerPlugInCustomPropertyDataTypeNone}};
static atomic_uint buffer_frames=256;
static atomic_uint driver_buffer_frames=256;
static atomic_uint buffer_size_read_log_count;
static atomic_uint buffer_range_read_log_count;
static atomic_uint active_io_frames;
static atomic_bool buffer_restart_pending;
static atomic_uint buffer_restart_state;

static AudioServerPlugInHostRef host;
static UInt32 references=1,seed=1;
static atomic_uint io_clients;
static UInt64 anchor_host_time;
static mach_timebase_info_data_t timebase;
static IONotificationPortRef usb_notifications;
static io_iterator_t usb_added,usb_removed;
static atomic_bool device_present;
static atomic_uint enqueue_failures;
static atomic_uint_fast64_t last_enqueue_log_ms;
static pthread_once_t usb_monitor_once=PTHREAD_ONCE_INIT;
static pthread_once_t default_output_monitor_once=PTHREAD_ONCE_INIT;
static pthread_mutex_t stream_lock=PTHREAD_MUTEX_INITIALIZER;
static bool transport_open;
static AudioStreamBasicDescription stream_format={SAMPLE_RATE,kAudioFormatLinearPCM,kAudioFormatFlagIsFloat|kAudioFormatFlagsNativeEndian|kAudioFormatFlagIsPacked,16,1,16,CHANNELS,32,0};

static HRESULT query_interface(void*,REFIID,LPVOID*); static ULONG add_ref(void*); static ULONG release_ref(void*);
static OSStatus initialize(AudioServerPlugInDriverRef,AudioServerPlugInHostRef); static OSStatus create_device(AudioServerPlugInDriverRef,CFDictionaryRef,const AudioServerPlugInClientInfo*,AudioObjectID*); static OSStatus destroy_device(AudioServerPlugInDriverRef,AudioObjectID);
static OSStatus add_client(AudioServerPlugInDriverRef,AudioObjectID,const AudioServerPlugInClientInfo*); static OSStatus remove_client(AudioServerPlugInDriverRef,AudioObjectID,const AudioServerPlugInClientInfo*); static OSStatus perform_change(AudioServerPlugInDriverRef,AudioObjectID,UInt64,void*); static OSStatus abort_change(AudioServerPlugInDriverRef,AudioObjectID,UInt64,void*);
static Boolean has_property(AudioServerPlugInDriverRef,AudioObjectID,pid_t,const AudioObjectPropertyAddress*); static OSStatus is_settable(AudioServerPlugInDriverRef,AudioObjectID,pid_t,const AudioObjectPropertyAddress*,Boolean*); static OSStatus property_size(AudioServerPlugInDriverRef,AudioObjectID,pid_t,const AudioObjectPropertyAddress*,UInt32,const void*,UInt32*); static OSStatus get_property(AudioServerPlugInDriverRef,AudioObjectID,pid_t,const AudioObjectPropertyAddress*,UInt32,const void*,UInt32,UInt32*,void*); static OSStatus set_property(AudioServerPlugInDriverRef,AudioObjectID,pid_t,const AudioObjectPropertyAddress*,UInt32,const void*,UInt32,const void*);
static OSStatus start_io(AudioServerPlugInDriverRef,AudioObjectID,UInt32); static OSStatus stop_io(AudioServerPlugInDriverRef,AudioObjectID,UInt32); static OSStatus zero_timestamp(AudioServerPlugInDriverRef,AudioObjectID,UInt32,Float64*,UInt64*,UInt64*); static OSStatus will_do(AudioServerPlugInDriverRef,AudioObjectID,UInt32,UInt32,Boolean*,Boolean*); static OSStatus begin_io(AudioServerPlugInDriverRef,AudioObjectID,UInt32,UInt32,UInt32,const AudioServerPlugInIOCycleInfo*); static OSStatus do_io(AudioServerPlugInDriverRef,AudioObjectID,AudioObjectID,UInt32,UInt32,UInt32,const AudioServerPlugInIOCycleInfo*,void*,void*); static OSStatus end_io(AudioServerPlugInDriverRef,AudioObjectID,UInt32,UInt32,UInt32,const AudioServerPlugInIOCycleInfo*);

static AudioServerPlugInDriverInterface interface={NULL,query_interface,add_ref,release_ref,initialize,create_device,destroy_device,add_client,remove_client,perform_change,abort_change,has_property,is_settable,property_size,get_property,set_property,start_io,stop_io,zero_timestamp,will_do,begin_io,do_io,end_io};
static AudioServerPlugInDriverInterface *interface_pointer=&interface;
static AudioServerPlugInDriverRef driver=&interface_pointer;

static bool usb_property_u16(io_registry_entry_t entry,CFStringRef key,UInt16 *out){CFTypeRef value=IORegistryEntryCreateCFProperty(entry,key,kCFAllocatorDefault,0);bool ok=value&&CFGetTypeID(value)==CFNumberGetTypeID()&&CFNumberGetValue(value,kCFNumberSInt16Type,out);if(value)CFRelease(value);return ok;}
static bool ns6_connected(void){io_iterator_t iterator=IO_OBJECT_NULL;io_service_t service;if(IOServiceGetMatchingServices(kIOMainPortDefault,IOServiceMatching("IOUSBHostDevice"),&iterator)!=KERN_SUCCESS)return false;bool found=false;while((service=IOIteratorNext(iterator))){UInt16 vendor=0,product=0;if(usb_property_u16(service,CFSTR(kUSBVendorID),&vendor)&&usb_property_u16(service,CFSTR(kUSBProductID),&product)&&vendor==0x15e4&&product==0x0079)found=true;IOObjectRelease(service);if(found)break;}IOObjectRelease(iterator);return found;}
static void publish_connection_state(void){
    bool connected=ns6_connected();
    if(!connected&&atomic_load_explicit(&device_present,memory_order_acquire)){
        /* USB enumeration can briefly lose the device while CoreAudio and the
           NS6 interfaces settle after boot. Don't withdraw the HAL device on
           one transient negative registry query. */
        const struct timespec settle={0,250000000};nanosleep(&settle,NULL);
        connected=ns6_connected();
    }
    bool previous=atomic_exchange_explicit(&device_present,connected,memory_order_acq_rel);
    if(previous==connected)return;
    os_log(OS_LOG_DEFAULT,"Numark NS6 USB presence changed: %{public}s (confirmed)",connected?"connected":"disconnected");
    if(!host)return;
    AudioObjectPropertyAddress plugin_change={kAudioPlugInPropertyDeviceList,kAudioObjectPropertyScopeGlobal,kAudioObjectPropertyElementMain};
    host->PropertiesChanged(host,kAudioObjectPlugInObject,1,&plugin_change);
    AudioObjectPropertyAddress device_change={kAudioDevicePropertyDeviceIsAlive,kAudioObjectPropertyScopeGlobal,kAudioObjectPropertyElementMain};
    host->PropertiesChanged(host,DEVICE_ID,1,&device_change);
}
static void usb_changed(void *reference,io_iterator_t iterator){(void)reference;io_service_t service;while((service=IOIteratorNext(iterator)))IOObjectRelease(service);publish_connection_state();}
static void *usb_monitor_thread(void *unused){(void)unused;usb_notifications=IONotificationPortCreate(kIOMainPortDefault);if(!usb_notifications)return NULL;CFRunLoopAddSource(CFRunLoopGetCurrent(),IONotificationPortGetRunLoopSource(usb_notifications),kCFRunLoopDefaultMode);if(IOServiceAddMatchingNotification(usb_notifications,kIOFirstMatchNotification,IOServiceMatching("IOUSBHostDevice"),usb_changed,NULL,&usb_added)==KERN_SUCCESS)usb_changed(NULL,usb_added);if(IOServiceAddMatchingNotification(usb_notifications,kIOTerminatedNotification,IOServiceMatching("IOUSBHostDevice"),usb_changed,NULL,&usb_removed)==KERN_SUCCESS)usb_changed(NULL,usb_removed);CFRunLoopRun();return NULL;}
static void monitor_usb(void){pthread_t thread;if(pthread_create(&thread,NULL,usb_monitor_thread,NULL)==0)pthread_detach(thread);}
static bool ns6_is_default_output(void){
    AudioObjectPropertyAddress address={kAudioHardwarePropertyDefaultOutputDevice,kAudioObjectPropertyScopeGlobal,kAudioObjectPropertyElementMain};
    AudioDeviceID device=0;UInt32 size=sizeof(device);
    if(AudioObjectGetPropertyData(kAudioObjectSystemObject,&address,0,NULL,&size,&device)!=noErr||!device)return false;
    address.mSelector=kAudioDevicePropertyDeviceUID;CFStringRef uid=NULL;size=sizeof(uid);
    if(AudioObjectGetPropertyData(device,&address,0,NULL,&size,&uid)!=noErr||!uid)return false;
    bool selected=CFStringCompare(uid,CFSTR("io.github.maurojuniorr.numark-ns6.device"),0)==kCFCompareEqualTo;
    CFRelease(uid);return selected;
}
static void *default_output_monitor_thread(void *unused){
    (void)unused;const struct timespec delay={0,500000000};
    bool previous_selected=false,first_check=true;
    for(;;){
        bool selected=atomic_load_explicit(&device_present,memory_order_acquire)&&ns6_is_default_output();
        pthread_mutex_lock(&stream_lock);
        if(first_check||selected!=previous_selected){
            os_log(OS_LOG_DEFAULT,"Numark NS6 default output route: %{public}s (USB present: %{public}s)",selected?"selected":"not selected",atomic_load_explicit(&device_present,memory_order_acquire)?"yes":"no");
            first_check=false;previous_selected=selected;
        }
        unsigned clients=atomic_load_explicit(&io_clients,memory_order_acquire);
        bool present=atomic_load_explicit(&device_present,memory_order_acquire);
        if(present&&!transport_open){
            if(ns6_usb_start()){
                transport_open=true;
                bool paused=clients==0;
                ns6_usb_set_paused(paused);
                os_log(OS_LOG_DEFAULT,"Numark NS6 USB audio clock started (%{public}s; default output: %{public}s)",paused?"silent standby":"active audio",selected?"yes":"no");
            }else os_log_error(OS_LOG_DEFAULT,"Numark NS6 USB audio standby initialization failed (default output: %{public}s)",selected?"yes":"no");
        }else if(transport_open){
            /* Keep the isochronous clock alive across route changes. Tearing
               down here joins the USB worker (including synchronous MIDI
               shutdown writes) while holding stream_lock; if IOKit stalls,
               the route monitor cannot observe the next selection and
               StartIO cannot resume the device. */
            bool paused=clients==0;
            ns6_usb_set_paused(paused);
        }
        pthread_mutex_unlock(&stream_lock);
        nanosleep(&delay,NULL);
    }
    return NULL;
}
static void monitor_default_output_once(void){pthread_t thread;if(pthread_create(&thread,NULL,default_output_monitor_thread,NULL)==0)pthread_detach(thread);}
static void monitor_default_output(void){pthread_once(&default_output_monitor_once,monitor_default_output_once);}

void *NS6_Create(CFAllocatorRef allocator,CFUUIDRef type){(void)allocator;return CFEqual(type,kAudioServerPlugInTypeUUID)?driver:NULL;}
static bool valid_object(AudioObjectID object){return object==kAudioObjectPlugInObject||object==DEVICE_ID||object==STREAM_ID;}
static bool supported_buffer_size(UInt32 frames);
static void *restart_usb_for_buffer(void *unused);
static HRESULT query_interface(void *value,REFIID uuid,LPVOID *out){if(value!=driver||!out)return E_NOINTERFACE;CFUUIDRef requested=CFUUIDCreateFromUUIDBytes(NULL,uuid);HRESULT result=E_NOINTERFACE;if(requested&&(CFEqual(requested,IUnknownUUID)||CFEqual(requested,kAudioServerPlugInDriverInterfaceUUID))){*out=driver;++references;result=0;}if(requested)CFRelease(requested);return result;}
static ULONG add_ref(void *value){return value==driver?++references:0;} static ULONG release_ref(void *value){return value==driver&&references?--references:0;}
static OSStatus initialize(AudioServerPlugInDriverRef value,AudioServerPlugInHostRef value_host){if(value!=driver)return kAudioHardwareBadObjectError;host=value_host;mach_timebase_info(&timebase);if(host->CopyFromStorage){CFPropertyListRef stored=NULL;if(host->CopyFromStorage(host,NS6_DRIVER_BUFFER_STORAGE_KEY,&stored)==noErr&&stored&&CFGetTypeID(stored)==CFNumberGetTypeID()){SInt32 frames=0;if(CFNumberGetValue((CFNumberRef)stored,kCFNumberSInt32Type,&frames)&&frames>0&&supported_buffer_size((UInt32)frames)){atomic_store(&driver_buffer_frames,(UInt32)frames);ns6_usb_set_startup_buffer_frames((UInt32)frames);}}if(stored)CFRelease(stored);}atomic_store(&device_present,ns6_connected());pthread_once(&usb_monitor_once,monitor_usb);monitor_default_output();return 0;}
static OSStatus create_device(AudioServerPlugInDriverRef d,CFDictionaryRef x,const AudioServerPlugInClientInfo*y,AudioObjectID*z){(void)d;(void)x;(void)y;(void)z;return kAudioHardwareUnsupportedOperationError;} static OSStatus destroy_device(AudioServerPlugInDriverRef d,AudioObjectID x){(void)d;(void)x;return kAudioHardwareUnsupportedOperationError;}
static NS6TrackedClient *find_client_locked(UInt32 client_id){for(size_t n=0;n<NS6_TRACKED_CLIENTS;++n)if(tracked_clients[n].used&&tracked_clients[n].client_id==client_id)return &tracked_clients[n];return NULL;}
static void refresh_client_identity_locked(void){NS6TrackedClient *latest=NULL;for(size_t n=0;n<NS6_TRACKED_CLIENTS;++n)if(tracked_clients[n].used&&tracked_clients[n].starts&&(!latest||tracked_clients[n].order>latest->order))latest=&tracked_clients[n];if(latest)os_log(OS_LOG_DEFAULT,"Numark NS6 active CoreAudio client: %{public}s pid=%d",latest->bundle_id,latest->pid);}
static OSStatus add_client(AudioServerPlugInDriverRef d,AudioObjectID object,const AudioServerPlugInClientInfo *info){(void)d;if(object!=DEVICE_ID||!info)return 0;char bundle_id[256]="unknown";if(info->mBundleID)CFStringGetCString(info->mBundleID,bundle_id,sizeof(bundle_id),kCFStringEncodingUTF8);pthread_mutex_lock(&stream_lock);NS6TrackedClient *slot=find_client_locked(info->mClientID);if(!slot)for(size_t n=0;n<NS6_TRACKED_CLIENTS;++n)if(!tracked_clients[n].used){slot=&tracked_clients[n];break;}if(slot){memset(slot,0,sizeof(*slot));slot->used=true;slot->client_id=info->mClientID;slot->pid=info->mProcessID;strlcpy(slot->bundle_id,bundle_id,sizeof(slot->bundle_id));}pthread_mutex_unlock(&stream_lock);os_log(OS_LOG_DEFAULT,"Numark NS6 CoreAudio client added: id=%u pid=%d bundle=%{public}s tracked=%{public}s",info->mClientID,info->mProcessID,bundle_id,slot?"yes":"no");return 0;}
static OSStatus remove_client(AudioServerPlugInDriverRef d,AudioObjectID object,const AudioServerPlugInClientInfo *info){(void)d;if(object!=DEVICE_ID||!info)return 0;pid_t pid=0;char bundle_id[256]="unknown";pthread_mutex_lock(&stream_lock);NS6TrackedClient *slot=find_client_locked(info->mClientID);if(slot){pid=slot->pid;strlcpy(bundle_id,slot->bundle_id,sizeof(bundle_id));memset(slot,0,sizeof(*slot));}refresh_client_identity_locked();pthread_mutex_unlock(&stream_lock);os_log(OS_LOG_DEFAULT,"Numark NS6 CoreAudio client removed: id=%u pid=%d bundle=%{public}s",info->mClientID,pid,bundle_id);return 0;}
static bool supported_buffer_size(UInt32 frames){for(size_t i=0;i<sizeof(supported_buffer_frames)/sizeof(supported_buffer_frames[0]);++i)if(supported_buffer_frames[i]==frames)return true;return false;}
static OSStatus perform_change(AudioServerPlugInDriverRef d,AudioObjectID object,UInt64 action,void *info){(void)d;(void)action;(void)info;return object==DEVICE_ID?0:kAudioHardwareBadObjectError;}
static OSStatus abort_change(AudioServerPlugInDriverRef d,AudioObjectID object,UInt64 action,void*info){(void)d;(void)action;(void)info;return object==DEVICE_ID?0:kAudioHardwareBadObjectError;}

static Boolean has_property(AudioServerPlugInDriverRef d,AudioObjectID object,pid_t pid,const AudioObjectPropertyAddress *address){
    (void)d;(void)pid;if(!valid_object(object)||!address)return false;
    switch(address->mSelector){case kAudioObjectPropertyBaseClass:case kAudioObjectPropertyClass:case kAudioObjectPropertyOwner:case kAudioObjectPropertyName:return true;default:break;}
    if(object==kAudioObjectPlugInObject)switch(address->mSelector){case kAudioObjectPropertyManufacturer:case kAudioObjectPropertyOwnedObjects:case kAudioPlugInPropertyDeviceList:case kAudioPlugInPropertyResourceBundle:return true;default:return false;}
    if(object==DEVICE_ID)switch(address->mSelector){case kAudioObjectPropertyManufacturer:case kAudioObjectPropertyOwnedObjects:case kAudioObjectPropertyCustomPropertyInfoList:case kAudioDevicePropertyDeviceUID:case kAudioDevicePropertyModelUID:case kAudioDevicePropertyTransportType:case kAudioDevicePropertyClockDomain:case kAudioDevicePropertyDeviceIsAlive:case kAudioDevicePropertyDeviceIsRunning:case kAudioDevicePropertyDeviceCanBeDefaultDevice:case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:case kAudioDevicePropertyLatency:case kAudioDevicePropertyStreams:case kAudioObjectPropertyControlList:case kAudioDevicePropertySafetyOffset:case kAudioDevicePropertyNominalSampleRate:case kAudioDevicePropertyAvailableNominalSampleRates:case kAudioDevicePropertyIsHidden:case kAudioDevicePropertyPreferredChannelsForStereo:case kAudioDevicePropertyBufferFrameSize:case kAudioDevicePropertyBufferFrameSizeRange:case kAudioDevicePropertyStreamConfiguration:case kAudioDevicePropertyZeroTimeStampPeriod:case NS6_DRIVER_BUFFER_PROPERTY:case NS6_BUFFER_RESTART_PROPERTY:case NS6_ACTIVE_IO_FRAMES_PROPERTY:case NS6_ACTIVE_CLIENT_PROPERTY:return true;default:return false;}
    switch(address->mSelector){case kAudioStreamPropertyIsActive:case kAudioStreamPropertyDirection:case kAudioStreamPropertyTerminalType:case kAudioStreamPropertyStartingChannel:case kAudioStreamPropertyLatency:case kAudioStreamPropertyVirtualFormat:case kAudioStreamPropertyPhysicalFormat:case kAudioStreamPropertyAvailableVirtualFormats:case kAudioStreamPropertyAvailablePhysicalFormats:return true;default:return false;}
}
static OSStatus is_settable(AudioServerPlugInDriverRef d,AudioObjectID o,pid_t p,const AudioObjectPropertyAddress*a,Boolean*out){if(!out)return kAudioHardwareIllegalOperationError;if(!has_property(d,o,p,a))return kAudioHardwareUnknownPropertyError;*out=o==DEVICE_ID&&(a->mSelector==NS6_DRIVER_BUFFER_PROPERTY||a->mSelector==kAudioDevicePropertyBufferFrameSize);return 0;}
static UInt32 scoped_count(const AudioObjectPropertyAddress *a){return a->mScope==kAudioObjectPropertyScopeInput?0:1;}
static OSStatus property_size(AudioServerPlugInDriverRef d,AudioObjectID object,pid_t pid,const AudioObjectPropertyAddress *a,UInt32 q,const void*x,UInt32 *out){
    (void)q;(void)x;if(!out)return kAudioHardwareIllegalOperationError;if(!has_property(d,object,pid,a))return kAudioHardwareUnknownPropertyError;
    switch(a->mSelector){
        case kAudioObjectPropertyName:case kAudioObjectPropertyManufacturer:case kAudioDevicePropertyDeviceUID:case kAudioDevicePropertyModelUID:case kAudioPlugInPropertyResourceBundle:*out=sizeof(CFStringRef);break;
        case kAudioObjectPropertyCustomPropertyInfoList:*out=object==DEVICE_ID?sizeof(custom_property_info):0;break;
        case NS6_DRIVER_BUFFER_PROPERTY:*out=sizeof(CFStringRef);break;
        case NS6_BUFFER_RESTART_PROPERTY:case NS6_ACTIVE_IO_FRAMES_PROPERTY:case NS6_ACTIVE_CLIENT_PROPERTY:*out=sizeof(CFStringRef);break;
        case kAudioObjectPropertyOwnedObjects:*out=(object==kAudioObjectPlugInObject&&!atomic_load(&device_present))?0:((object==kAudioObjectPlugInObject||object==DEVICE_ID)?sizeof(AudioObjectID):0);break;
        case kAudioPlugInPropertyDeviceList:*out=atomic_load(&device_present)?sizeof(AudioObjectID):0;break;
        case kAudioDevicePropertyRelatedDevices:*out=sizeof(AudioObjectID);break;
        case kAudioDevicePropertyStreams:*out=scoped_count(a)*sizeof(AudioObjectID);break;
        case kAudioObjectPropertyControlList:*out=0;break;
        case kAudioDevicePropertyNominalSampleRate:*out=sizeof(Float64);break;
        case kAudioDevicePropertyAvailableNominalSampleRates:case kAudioDevicePropertyBufferFrameSizeRange:*out=sizeof(AudioValueRange);break;
        case kAudioDevicePropertyPreferredChannelsForStereo:*out=2*sizeof(UInt32);break;
        case kAudioDevicePropertyStreamConfiguration:*out=offsetof(AudioBufferList,mBuffers)+sizeof(AudioBuffer);break;
        case kAudioStreamPropertyVirtualFormat:case kAudioStreamPropertyPhysicalFormat:*out=sizeof(AudioStreamBasicDescription);break;
        case kAudioStreamPropertyAvailableVirtualFormats:case kAudioStreamPropertyAvailablePhysicalFormats:*out=sizeof(AudioStreamRangedDescription);break;
        default:*out=sizeof(UInt32);break;
    }return 0;
}
static CFStringRef string_property(AudioObjectID object,AudioObjectPropertySelector selector){
    if(selector==kAudioObjectPropertyManufacturer)return CFSTR("Numark / community driver");
    if(selector==kAudioDevicePropertyDeviceUID)return CFSTR("io.github.maurojuniorr.numark-ns6.device");
    if(selector==kAudioDevicePropertyModelUID)return CFSTR("io.github.maurojuniorr.numark-ns6.original");
    if(selector==kAudioPlugInPropertyResourceBundle)return CFSTR("");
    if(object==kAudioObjectPlugInObject)return CFSTR("Numark NS6 Plug-In");
    if(object==DEVICE_ID)return CFSTR("Numark NS6"); return CFSTR("Numark NS6 Output");
}
static OSStatus get_property(AudioServerPlugInDriverRef d,AudioObjectID object,pid_t pid,const AudioObjectPropertyAddress *a,UInt32 q,const void*x,UInt32 input_size,UInt32 *output_size,void *output){
    UInt32 needed=0;OSStatus status=property_size(d,object,pid,a,q,x,&needed);if(status)return status;if(input_size<needed)return kAudioHardwareBadPropertySizeError;if(output_size)*output_size=needed;if(!needed)return 0;if(!output)return kAudioHardwareIllegalOperationError;
    switch(a->mSelector){
        case kAudioObjectPropertyName:case kAudioObjectPropertyManufacturer:case kAudioDevicePropertyDeviceUID:case kAudioDevicePropertyModelUID:case kAudioPlugInPropertyResourceBundle:*(CFStringRef*)output=CFRetain(string_property(object,a->mSelector));return 0;
        case kAudioObjectPropertyCustomPropertyInfoList:if(object==DEVICE_ID)memcpy(output,custom_property_info,sizeof(custom_property_info));return 0;
        case NS6_DRIVER_BUFFER_PROPERTY:*(CFStringRef*)output=CFStringCreateWithFormat(kCFAllocatorDefault,NULL,CFSTR("%u"),atomic_load_explicit(&driver_buffer_frames,memory_order_acquire));return *(CFStringRef*)output?0:kAudioHardwareUnspecifiedError;
        case NS6_BUFFER_RESTART_PROPERTY:{UInt32 state=atomic_load_explicit(&buffer_restart_state,memory_order_acquire);const char *text=state==1?"restarting":state==2?"restarted":state==3?"failed":state==4?"applied":"idle";*(CFStringRef*)output=CFStringCreateWithCString(kCFAllocatorDefault,text,kCFStringEncodingUTF8);return *(CFStringRef*)output?0:kAudioHardwareUnspecifiedError;}
        case NS6_ACTIVE_IO_FRAMES_PROPERTY:*(CFStringRef*)output=CFStringCreateWithFormat(kCFAllocatorDefault,NULL,CFSTR("%u"),atomic_load_explicit(&active_io_frames,memory_order_acquire));return *(CFStringRef*)output?0:kAudioHardwareUnspecifiedError;
        case NS6_ACTIVE_CLIENT_PROPERTY:{pid_t active_pid=0;char bundle_id[256]={0};pthread_mutex_lock(&stream_lock);NS6TrackedClient *latest=NULL;for(size_t n=0;n<NS6_TRACKED_CLIENTS;++n)if(tracked_clients[n].used&&tracked_clients[n].starts&&(!latest||tracked_clients[n].order>latest->order))latest=&tracked_clients[n];if(latest){active_pid=latest->pid;memcpy(bundle_id,latest->bundle_id,sizeof(bundle_id));}pthread_mutex_unlock(&stream_lock);*(CFStringRef*)output=CFStringCreateWithFormat(kCFAllocatorDefault,NULL,CFSTR("%d|%s"),active_pid,bundle_id);return *(CFStringRef*)output?0:kAudioHardwareUnspecifiedError;}
        case kAudioObjectPropertyBaseClass:*(AudioClassID*)output=kAudioObjectClassID;return 0;
        case kAudioObjectPropertyClass:*(AudioClassID*)output=object==kAudioObjectPlugInObject?kAudioPlugInClassID:(object==DEVICE_ID?kAudioDeviceClassID:kAudioStreamClassID);return 0;
        case kAudioObjectPropertyOwner:*(AudioObjectID*)output=object==STREAM_ID?DEVICE_ID:kAudioObjectUnknown;return 0;
        case kAudioObjectPropertyOwnedObjects:*(AudioObjectID*)output=object==kAudioObjectPlugInObject?DEVICE_ID:STREAM_ID;return 0;
        case kAudioPlugInPropertyDeviceList:if(atomic_load(&device_present))*(AudioObjectID*)output=DEVICE_ID;return 0;
        case kAudioDevicePropertyRelatedDevices:*(AudioObjectID*)output=DEVICE_ID;return 0;
        case kAudioDevicePropertyStreams:*(AudioObjectID*)output=STREAM_ID;return 0;
        case kAudioDevicePropertyNominalSampleRate:*(Float64*)output=SAMPLE_RATE;return 0;
        case kAudioDevicePropertyAvailableNominalSampleRates:{AudioValueRange r={SAMPLE_RATE,SAMPLE_RATE};*(AudioValueRange*)output=r;return 0;}
        case kAudioDevicePropertyBufferFrameSizeRange:{AudioValueRange r={supported_buffer_frames[0],supported_buffer_frames[sizeof(supported_buffer_frames)/sizeof(supported_buffer_frames[0])-1]};*(AudioValueRange*)output=r;unsigned log_count=atomic_fetch_add_explicit(&buffer_range_read_log_count,1,memory_order_relaxed);if(log_count<16)os_log(OS_LOG_DEFAULT,"Numark NS6 buffer range callback: pid %d scope 0x%x element %u => %.0f..%.0f frames",pid,a->mScope,a->mElement,r.mMinimum,r.mMaximum);return 0;}
        case kAudioDevicePropertyPreferredChannelsForStereo:((UInt32*)output)[0]=1;((UInt32*)output)[1]=2;return 0;
        case kAudioDevicePropertyStreamConfiguration:{AudioBufferList *list=output;list->mNumberBuffers=1;list->mBuffers[0].mNumberChannels=scoped_count(a)?CHANNELS:0;list->mBuffers[0].mDataByteSize=0;list->mBuffers[0].mData=NULL;return 0;}
        case kAudioStreamPropertyVirtualFormat:case kAudioStreamPropertyPhysicalFormat:*(AudioStreamBasicDescription*)output=stream_format;return 0;
        case kAudioStreamPropertyAvailableVirtualFormats:case kAudioStreamPropertyAvailablePhysicalFormats:{AudioStreamRangedDescription r={stream_format,{SAMPLE_RATE,SAMPLE_RATE}};*(AudioStreamRangedDescription*)output=r;return 0;}
        default:break;
    }
    UInt32 value=0;
    switch(a->mSelector){case kAudioDevicePropertyTransportType:value=kAudioDeviceTransportTypeUSB;break;case kAudioDevicePropertyDeviceIsAlive:value=atomic_load(&device_present)?1:0;break;case kAudioDevicePropertyDeviceCanBeDefaultDevice:case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:case kAudioStreamPropertyIsActive:value=1;break;case kAudioDevicePropertyDeviceIsRunning:value=atomic_load(&io_clients)?1:0;break;case kAudioDevicePropertyBufferFrameSize:value=atomic_load_explicit(&buffer_frames,memory_order_acquire);break;case kAudioDevicePropertyZeroTimeStampPeriod:value=NS6_ZERO_TIMESTAMP_PERIOD;break;case kAudioStreamPropertyDirection:value=0;break;case kAudioStreamPropertyTerminalType:value=kAudioStreamTerminalTypeLine;break;case kAudioStreamPropertyStartingChannel:value=1;break;default:value=0;break;}*(UInt32*)output=value;if(a->mSelector==kAudioDevicePropertyBufferFrameSize){unsigned log_count=atomic_fetch_add_explicit(&buffer_size_read_log_count,1,memory_order_relaxed);if(log_count<16)os_log(OS_LOG_DEFAULT,"Numark NS6 buffer size callback: pid %d scope 0x%x element %u => %u frames",pid,a->mScope,a->mElement,value);}return 0;
}
static OSStatus set_property(AudioServerPlugInDriverRef d,AudioObjectID o,pid_t p,const AudioObjectPropertyAddress*a,UInt32 q,const void*x,UInt32 n,const void*v){(void)d;(void)q;(void)x;if(o!=DEVICE_ID||!a)return kAudioHardwareUnsupportedOperationError;
    if(a->mSelector==kAudioDevicePropertyBufferFrameSize){
        if((a->mScope!=kAudioObjectPropertyScopeGlobal&&a->mScope!=kAudioObjectPropertyScopeOutput)||a->mElement!=kAudioObjectPropertyElementMain||n!=sizeof(UInt32)||!v){os_log(OS_LOG_DEFAULT,"Numark NS6 buffer size setter rejected malformed request: pid %d scope 0x%x element %u bytes %u",p,a->mScope,a->mElement,n);return kAudioHardwareBadPropertySizeError;}
        UInt32 frames=*(const UInt32*)v;
        if(!ns6_buffer_frame_size_request_valid(frames,supported_buffer_frames[0],supported_buffer_frames[sizeof(supported_buffer_frames)/sizeof(supported_buffer_frames[0])-1])){os_log(OS_LOG_DEFAULT,"Numark NS6 buffer size setter rejected value: pid %d scope 0x%x element %u requested %u allowed %u..%u",p,a->mScope,a->mElement,frames,supported_buffer_frames[0],supported_buffer_frames[sizeof(supported_buffer_frames)/sizeof(supported_buffer_frames[0])-1]);return kAudioHardwareIllegalOperationError;}
        UInt32 previous=atomic_exchange_explicit(&buffer_frames,frames,memory_order_acq_rel);
        os_log(OS_LOG_DEFAULT,"Numark NS6 CoreAudio buffer frame size set: requested %u, previous %u, client pid %d",frames,previous,p);
        if(host){AudioObjectPropertyAddress changed={kAudioDevicePropertyBufferFrameSize,kAudioObjectPropertyScopeGlobal,kAudioObjectPropertyElementMain};host->PropertiesChanged(host,o,1,&changed);}
        return noErr;
    }
    if(a->mSelector!=NS6_DRIVER_BUFFER_PROPERTY)return kAudioHardwareUnsupportedOperationError;
    if(a->mScope!=kAudioObjectPropertyScopeGlobal||a->mElement!=kAudioObjectPropertyElementMain||n!=sizeof(CFStringRef)||!v)return kAudioHardwareBadPropertySizeError;CFStringRef text=*(CFStringRef const*)v;if(!text)return kAudioHardwareIllegalOperationError;SInt32 value=CFStringGetIntValue(text);if(value<0||!supported_buffer_size((UInt32)value))return kAudioHardwareIllegalOperationError;UInt32 frames=(UInt32)value;if(atomic_load_explicit(&buffer_restart_pending,memory_order_acquire))return kAudioHardwareIllegalOperationError;if(!host||!host->WriteToStorage)return kAudioHardwareUnspecifiedError;CFNumberRef stored=CFNumberCreate(kCFAllocatorDefault,kCFNumberSInt32Type,&value);if(!stored)return kAudioHardwareUnspecifiedError;OSStatus status=host->WriteToStorage(host,NS6_DRIVER_BUFFER_STORAGE_KEY,stored);CFRelease(stored);if(status!=noErr)return status;atomic_store_explicit(&driver_buffer_frames,frames,memory_order_release);ns6_usb_set_startup_buffer_frames(frames);atomic_store_explicit(&buffer_restart_pending,true,memory_order_release);atomic_store_explicit(&buffer_restart_state,1,memory_order_release);pthread_t thread;if(pthread_create(&thread,NULL,restart_usb_for_buffer,NULL)!=0){atomic_store_explicit(&buffer_restart_pending,false,memory_order_release);atomic_store_explicit(&buffer_restart_state,3,memory_order_release);return kAudioHardwareUnspecifiedError;}pthread_detach(thread);os_log(OS_LOG_DEFAULT,"Numark NS6 applying %u-frame startup prebuffer and restarting USB audio transport",frames);AudioObjectPropertyAddress changed[2]={{NS6_DRIVER_BUFFER_PROPERTY,kAudioObjectPropertyScopeGlobal,kAudioObjectPropertyElementMain},{NS6_BUFFER_RESTART_PROPERTY,kAudioObjectPropertyScopeGlobal,kAudioObjectPropertyElementMain}};host->PropertiesChanged(host,o,2,changed);return noErr;}

static void *restart_usb_for_buffer(void *unused){(void)unused;pthread_mutex_lock(&stream_lock);bool was_open=transport_open;unsigned clients=atomic_load_explicit(&io_clients,memory_order_acquire);pthread_mutex_unlock(&stream_lock);bool success=true;if(was_open){ns6_usb_set_paused(true);success=atomic_load_explicit(&device_present,memory_order_acquire)&&ns6_usb_request_restart();if(success){const struct timespec delay={0,50000000};for(unsigned attempt=0;attempt<200&&!ns6_usb_is_ready();++attempt){if(!atomic_load_explicit(&device_present,memory_order_acquire)){success=false;break;}nanosleep(&delay,NULL);}success=success&&ns6_usb_is_ready();}if(success)ns6_usb_set_paused(clients==0);}atomic_store_explicit(&buffer_restart_state,success?(was_open?2:4):3,memory_order_release);atomic_store_explicit(&buffer_restart_pending,false,memory_order_release);if(success)os_log(OS_LOG_DEFAULT,"Numark NS6 USB transport buffer change %s",was_open?"restarted":"applied while transport idle");else os_log_error(OS_LOG_DEFAULT,"Numark NS6 USB transport restart for buffer change failed");if(host){AudioObjectPropertyAddress changed[2]={{NS6_DRIVER_BUFFER_PROPERTY,kAudioObjectPropertyScopeGlobal,kAudioObjectPropertyElementMain},{NS6_BUFFER_RESTART_PROPERTY,kAudioObjectPropertyScopeGlobal,kAudioObjectPropertyElementMain}};host->PropertiesChanged(host,DEVICE_ID,2,changed);}return NULL;}

static OSStatus start_io(AudioServerPlugInDriverRef d,AudioObjectID object,UInt32 client){
    (void)d;
    if(object!=DEVICE_ID)return kAudioHardwareBadObjectError;
    if(!atomic_load(&device_present)){os_log_error(OS_LOG_DEFAULT,"Numark NS6 start_io refused: USB device absent");return kAudioHardwareNotRunningError;}
    pthread_mutex_lock(&stream_lock);
    unsigned clients=atomic_load_explicit(&io_clients,memory_order_acquire);
    if(!transport_open&&!ns6_usb_start()){pthread_mutex_unlock(&stream_lock);os_log_error(OS_LOG_DEFAULT,"Numark NS6 start_io failed: USB transport initialization failed");return kAudioHardwareUnspecifiedError;}
    NS6TrackedClient *active_client=find_client_locked(client);
    if(active_client){++active_client->starts;active_client->order=++client_order;refresh_client_identity_locked();}
    transport_open=true;
    /* CoreAudio can restart a device while another client still holds an
       I/O context. Always clear silent-pause on StartIO, not only on the
       transition from zero clients. */
    ns6_usb_set_paused(false);
    if(clients==0){
        anchor_host_time=mach_absolute_time();++seed;atomic_store(&enqueue_failures,0);atomic_store(&last_enqueue_log_ms,0);
    }
    atomic_fetch_add_explicit(&io_clients,1,memory_order_release);
    os_log(OS_LOG_DEFAULT,"Numark NS6 audio stream resumed (sample rate %.0f, client %u; %u prior clients)",SAMPLE_RATE,client,clients);
    pthread_mutex_unlock(&stream_lock);
    return 0;
}
static OSStatus stop_io(AudioServerPlugInDriverRef d,AudioObjectID object,UInt32 client){
    (void)d;
    if(object!=DEVICE_ID)return kAudioHardwareBadObjectError;
    pthread_mutex_lock(&stream_lock);
    unsigned clients=atomic_load_explicit(&io_clients,memory_order_acquire);
    NS6TrackedClient *active_client=find_client_locked(client);
    if(active_client&&active_client->starts)--active_client->starts;
    if(clients){
        clients=atomic_fetch_sub_explicit(&io_clients,1,memory_order_acq_rel)-1;
        if(!clients){
            atomic_store_explicit(&active_io_frames,0,memory_order_release);
            ns6_usb_set_paused(true);ns6_transport_discard();
            os_log(OS_LOG_DEFAULT,"Numark NS6 audio paused; keeping USB clock active with silence (client %u)",client);
        }
    }
    refresh_client_identity_locked();
    pthread_mutex_unlock(&stream_lock);
    return 0;
}
static OSStatus zero_timestamp(AudioServerPlugInDriverRef d,AudioObjectID object,UInt32 client,Float64 *sample,UInt64 *host_time,UInt64 *out_seed){(void)d;(void)client;if(object!=DEVICE_ID||!sample||!host_time||!out_seed)return kAudioHardwareIllegalOperationError;NS6ZeroTimestamp value=ns6_zero_timestamp_calculate(anchor_host_time,mach_absolute_time(),timebase.numer,timebase.denom,SAMPLE_RATE);*sample=(Float64)value.sample_time;*host_time=value.host_time;*out_seed=seed;return 0;}
static OSStatus will_do(AudioServerPlugInDriverRef d,AudioObjectID object,UInt32 client,UInt32 operation,Boolean *will,Boolean *in_place){(void)d;(void)object;(void)client;if(!will||!in_place)return kAudioHardwareIllegalOperationError;*will=operation==kAudioServerPlugInIOOperationWriteMix;*in_place=true;return 0;}
static OSStatus begin_io(AudioServerPlugInDriverRef d,AudioObjectID o,UInt32 c,UInt32 op,UInt32 frames,const AudioServerPlugInIOCycleInfo*i){(void)d;(void)c;(void)i;if(o==DEVICE_ID&&op==kAudioServerPlugInIOOperationWriteMix&&frames)atomic_store_explicit(&active_io_frames,frames,memory_order_release);return 0;}
static OSStatus do_io(AudioServerPlugInDriverRef d,AudioObjectID object,AudioObjectID stream,UInt32 client,UInt32 operation,UInt32 frames,const AudioServerPlugInIOCycleInfo*i,void *main_buffer,void *secondary){
    (void)d;(void)client;(void)i;(void)secondary;
    if(object!=DEVICE_ID||stream!=STREAM_ID)return kAudioHardwareBadObjectError;
    if(operation==kAudioServerPlugInIOOperationWriteMix){
        atomic_store_explicit(&active_io_frames,frames,memory_order_release);
        bool accepted=ns6_transport_enqueue(main_buffer,frames,&stream_format);
        if(!accepted){
            unsigned failures=atomic_fetch_add(&enqueue_failures,1)+1;
            uint64_t now=mach_absolute_time();double now_ms=(double)now*timebase.numer/timebase.denom/1e6;
            uint_fast64_t previous=atomic_load_explicit(&last_enqueue_log_ms,memory_order_relaxed);
            if(failures==1||(now_ms-previous>=1000&&atomic_compare_exchange_strong_explicit(&last_enqueue_log_ms,&previous,(uint_fast64_t)now_ms,memory_order_relaxed,memory_order_relaxed)))
                os_log_error(OS_LOG_DEFAULT,"Numark NS6 rejected CoreAudio buffer: %u cumulative failures, %u input frames, %u queued USB frames",failures,frames,ns6_transport_available());
            return kAudioHardwareUnspecifiedError;
        }
    }
    return 0;
}
static OSStatus end_io(AudioServerPlugInDriverRef d,AudioObjectID o,UInt32 c,UInt32 op,UInt32 frames,const AudioServerPlugInIOCycleInfo*i){(void)d;(void)o;(void)c;(void)op;(void)frames;(void)i;return 0;}
