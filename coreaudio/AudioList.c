#include <CoreAudio/CoreAudio.h>
#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc,char **argv){
    AudioObjectPropertyAddress address={kAudioHardwarePropertyDevices,kAudioObjectPropertyScopeGlobal,kAudioObjectPropertyElementMain};
    UInt32 bytes=0;OSStatus status=AudioObjectGetPropertyDataSize(kAudioObjectSystemObject,&address,0,NULL,&bytes);
    if(status){fprintf(stderr,"device list size: %d\n",status);return 1;}
    AudioDeviceID *devices=malloc(bytes);status=AudioObjectGetPropertyData(kAudioObjectSystemObject,&address,0,NULL,&bytes,devices);
    if(status){fprintf(stderr,"device list: %d\n",status);return 1;}
    UInt32 ns6=0;
    for(UInt32 i=0;i<bytes/sizeof(*devices);++i){
        CFStringRef name=NULL,uid=NULL;UInt32 size=sizeof(name);AudioObjectPropertyAddress n={kAudioObjectPropertyName,kAudioObjectPropertyScopeGlobal,kAudioObjectPropertyElementMain};
        char text[256]="?",uid_text[256]="?";UInt32 frames=0,zero_period=0,reported_latency=0,safety_offset=0;
        if(!AudioObjectGetPropertyData(devices[i],&n,0,NULL,&size,&name)&&name){CFStringGetCString(name,text,sizeof(text),kCFStringEncodingUTF8);CFRelease(name);}
        n.mSelector=kAudioDevicePropertyDeviceUID;size=sizeof(uid);if(!AudioObjectGetPropertyData(devices[i],&n,0,NULL,&size,&uid)&&uid){CFStringGetCString(uid,uid_text,sizeof(uid_text),kCFStringEncodingUTF8);CFRelease(uid);}
        n.mSelector=kAudioDevicePropertyBufferFrameSize;n.mScope=kAudioObjectPropertyScopeOutput;size=sizeof(frames);AudioObjectGetPropertyData(devices[i],&n,0,NULL,&size,&frames);
        n.mSelector=kAudioDevicePropertyZeroTimeStampPeriod;n.mScope=kAudioObjectPropertyScopeGlobal;size=sizeof(zero_period);AudioObjectGetPropertyData(devices[i],&n,0,NULL,&size,&zero_period);
        n.mSelector=kAudioDevicePropertyLatency;size=sizeof(reported_latency);AudioObjectGetPropertyData(devices[i],&n,0,NULL,&size,&reported_latency);
        n.mSelector=kAudioDevicePropertySafetyOffset;size=sizeof(safety_offset);AudioObjectGetPropertyData(devices[i],&n,0,NULL,&size,&safety_offset);
        printf("%u %s | uid=%s | I/O buffer=%u | zero period=%u | device latency=%u | safety offset=%u\n",devices[i],text,uid_text,frames,zero_period,reported_latency,safety_offset);
        if(strcmp(uid_text,"io.github.maurojuniorr.numark-ns6.device")==0)ns6=devices[i];
    }
    if(argc>1&&ns6){char *end=NULL;unsigned long parsed=strtoul(argv[1],&end,10);if(!end||*end||parsed>UINT32_MAX){fprintf(stderr,"usage: %s [buffer-frames]\n",argv[0]);free(devices);return 2;}UInt32 requested=(UInt32)parsed;AudioObjectPropertyAddress b={kAudioDevicePropertyBufferFrameSize,kAudioObjectPropertyScopeGlobal,kAudioObjectPropertyElementMain};OSStatus set=AudioObjectSetPropertyData(ns6,&b,0,NULL,sizeof(requested),&requested);printf("set buffer %u: OSStatus %d\n",requested,set);sleep(2);UInt32 actual=0,actual_size=sizeof(actual);OSStatus get=AudioObjectGetPropertyData(ns6,&b,0,NULL,&actual_size,&actual);printf("read buffer: OSStatus %d, %u frames\n",get,actual);}
    free(devices);return 0;
}
