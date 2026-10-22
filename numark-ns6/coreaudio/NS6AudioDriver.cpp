#include "NS6AudioDriver.hpp"

#include "AudioDeviceLifecycle.hpp"
#include "AudioOutputEngine.hpp"
#include "AudioInputEngine.hpp"
#include "AudioTransportRuntime.hpp"
#include "../audio-in/AudioInputDecoder.hpp"
#include "../audio-in/AudioInputReader.hpp"
#include "../midi/ControllerActivation.hpp"

#include <CoreFoundation/CoreFoundation.h>
#include <CoreAudio/AudioHardware.h>
#include <mach/mach_time.h>
#include <os/log.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <new>
#include <memory>

OSStatus NS6AudioDriver::processOutputMix(AudioOutputEngine& engine, UInt32 operation,
                                          UInt32 frames,
                                          const Float32* samples) noexcept {
    if (operation != kAudioServerPlugInIOOperationWriteMix) return noErr;
    if (frames > UINT32_MAX / kBytesPerFrame || (frames != 0 && samples == nullptr))
        return kAudioHardwareIllegalOperationError;
    engine.writeMix(samples, frames);
    return noErr;
}

OSStatus NS6AudioDriver::processInput(AudioInputEngine& engine, UInt32 operation,
                                      UInt32 frames, Float32* samples) noexcept {
    if (operation != kAudioServerPlugInIOOperationReadInput) return noErr;
    if (frames > UINT32_MAX / (kInputChannels * sizeof(Float32)) ||
        (frames != 0 && samples == nullptr))
        return kAudioHardwareIllegalOperationError;
    engine.readInput(samples, frames);
    return noErr;
}

namespace {
constexpr AudioObjectID kPluginObjectID = kAudioObjectPlugInObject;
constexpr UInt32 kRingCapacityFrames = 16384;
constexpr UInt32 kInputMaxTransferFrames = AudioInputReader::kTransferBytes /
                                           AudioInputDecoder::kSourceStrideBytes;
constexpr UInt32 kIOBufferFrames = 512;
constexpr UInt32 kDeviceLatencyFrames = 256;
const CFStringRef kDeviceUID = CFSTR("io.github.maurojuniorr.numark-ns6.out");
const CFStringRef kInputDeviceUID = CFSTR("io.github.maurojuniorr.numark-ns6.in");
const CFStringRef kModelUID = CFSTR("io.github.maurojuniorr.numark-ns6");
const CFStringRef kPluginBundleID = CFSTR("io.github.maurojuniorr.NumarkNS6.AudioServer");

struct DriverState {
    AudioServerPlugInHostRef host = nullptr;
    std::mutex mutex;
    AudioDeviceLifecycle outputLifecycle;
    AudioDeviceLifecycle inputLifecycle;
    AudioOutputEngine output{kRingCapacityFrames};
    AudioInputEngine input{kRingCapacityFrames, kInputMaxTransferFrames};
    std::unique_ptr<NS6DeviceSession> session;
    std::unique_ptr<AudioTransportRuntime> runtime;
    struct Client { AudioObjectID device; UInt32 id; } clients[64]{};
    UInt32 clientCount = 0;
    std::atomic<UInt32> references{1};
    std::atomic<bool> running{false};
    UInt64 timestampSeed = 1;
} gState;

AudioServerPlugInDriverInterface gInterface;
AudioServerPlugInDriverInterface* gInterfacePointer = &gInterface;
AudioServerPlugInDriverRef gDriver = &gInterfacePointer;
IOReturn stopHardware();

bool validObject(AudioObjectID id) noexcept {
    return id == kPluginObjectID || id == NS6AudioDriver::kDeviceObjectID ||
           id == NS6AudioDriver::kInputDeviceObjectID ||
           id == NS6AudioDriver::kOutputStreamObjectID ||
           id == NS6AudioDriver::kInputStreamObjectID;
}

bool validDevice(AudioObjectID id) noexcept {
    return id == NS6AudioDriver::kDeviceObjectID || id == NS6AudioDriver::kInputDeviceObjectID;
}

AudioDeviceLifecycle& lifecycle(AudioObjectID device) noexcept {
    return device == NS6AudioDriver::kDeviceObjectID ? gState.outputLifecycle : gState.inputLifecycle;
}

bool anyActive() noexcept {
    return gState.outputLifecycle.activeStarts() != 0 || gState.inputLifecycle.activeStarts() != 0;
}

bool hasClient(AudioObjectID device, UInt32 id) noexcept {
    for (UInt32 i = 0; i < gState.clientCount; ++i)
        if (gState.clients[i].device == device && gState.clients[i].id == id) return true;
    return false;
}

HRESULT STDMETHODCALLTYPE queryInterface(void* driver, REFIID uuid, LPVOID* out) {
    if (driver != gDriver || out == nullptr) return E_NOINTERFACE;
    *out = nullptr;
    CFUUIDRef requested = CFUUIDCreateFromUUIDBytes(kCFAllocatorDefault, uuid);
    if (requested == nullptr) return E_NOINTERFACE;
    const bool matches = CFEqual(requested, IUnknownUUID) ||
                         CFEqual(requested, kAudioServerPlugInDriverInterfaceUUID);
    CFRelease(requested);
    if (!matches) return E_NOINTERFACE;
    *out = gDriver;
    gState.references.fetch_add(1);
    return S_OK;
}

ULONG STDMETHODCALLTYPE addRef(void*) { return gState.references.fetch_add(1) + 1; }
ULONG STDMETHODCALLTYPE release(void*) {
    UInt32 count = gState.references.load();
    while (count > 1 && !gState.references.compare_exchange_weak(count, count - 1)) {}
    return gState.references.load();
}

OSStatus STDMETHODCALLTYPE initialize(AudioServerPlugInDriverRef, AudioServerPlugInHostRef host) {
    if (host == nullptr || host->PropertiesChanged == nullptr)
        return kAudioHardwareIllegalOperationError;
    gState.host = host;
    const AudioObjectPropertyAddress address{kAudioPlugInPropertyDeviceList,
        kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    const OSStatus result = host->PropertiesChanged(host, kPluginObjectID, 1, &address);
    if (result != noErr) gState.host = nullptr;
    os_log_info(OS_LOG_DEFAULT, "NS6 Audio Initialize PropertiesChanged status=%{public}d", result);
    return result;
}

OSStatus STDMETHODCALLTYPE createDevice(AudioServerPlugInDriverRef, CFDictionaryRef,
                                         const AudioServerPlugInClientInfo*, AudioObjectID* out) {
    if (out == nullptr) return kAudioHardwareIllegalOperationError;
    return kAudioHardwareUnsupportedOperationError;
}
OSStatus STDMETHODCALLTYPE destroyDevice(AudioServerPlugInDriverRef, AudioObjectID) {
    return kAudioHardwareUnsupportedOperationError;
}

OSStatus STDMETHODCALLTYPE addClient(AudioServerPlugInDriverRef, AudioObjectID device,
                                      const AudioServerPlugInClientInfo* info) {
    if (!validDevice(device) || info == nullptr)
        return kAudioHardwareBadObjectError;
    std::lock_guard<std::mutex> lock(gState.mutex);
    if (hasClient(device, info->mClientID)) return noErr;
    if (gState.clientCount == 64) return kAudioHardwareNotReadyError;
    gState.clients[gState.clientCount++] = {device, info->mClientID};
    return noErr;
}

void notifyRunningChanged(AudioObjectID device) {
    if (gState.host == nullptr) return;
    const AudioObjectPropertyAddress address{kAudioDevicePropertyDeviceIsRunning,
        kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    gState.host->PropertiesChanged(gState.host, device, 1, &address);
}

OSStatus STDMETHODCALLTYPE removeClient(AudioServerPlugInDriverRef, AudioObjectID device,
                                         const AudioServerPlugInClientInfo* info) {
    if (!validDevice(device) || info == nullptr)
        return kAudioHardwareBadObjectError;
    std::unique_lock<std::mutex> lock(gState.mutex);
    for (UInt32 i = 0; i < gState.clientCount; ++i) {
        if (gState.clients[i].device == device && gState.clients[i].id == info->mClientID) {
            const auto transition = lifecycle(device).removeClient(info->mClientID);
            gState.clients[i] = gState.clients[--gState.clientCount];
            if (transition == AudioIOTransition::LastClientStopped) {
                const IOReturn result = anyActive() ? kIOReturnSuccess : stopHardware();
                lock.unlock();
                if (result == kIOReturnSuccess) notifyRunningChanged(device);
                return result;
            }
            break;
        }
    }
    return noErr;
}

OSStatus STDMETHODCALLTYPE performConfiguration(AudioServerPlugInDriverRef, AudioObjectID,
                                                  UInt64, void*) {
    return kAudioHardwareUnsupportedOperationError;
}
OSStatus STDMETHODCALLTYPE abortConfiguration(AudioServerPlugInDriverRef, AudioObjectID,
                                                UInt64, void*) { return noErr; }

bool hasProperty(AudioObjectID object, const AudioObjectPropertyAddress& a) {
    if (!validObject(object)) return false;
    if (validDevice(object) &&
        a.mSelector == kAudioDevicePropertyStreams &&
        a.mScope != kAudioObjectPropertyScopeInput &&
        a.mScope != kAudioObjectPropertyScopeOutput &&
        a.mScope != kAudioObjectPropertyScopePlayThrough) return false;
    if ((object == NS6AudioDriver::kOutputStreamObjectID ||
         object == NS6AudioDriver::kInputStreamObjectID) &&
        a.mScope != kAudioObjectPropertyScopeGlobal) return false;
    switch (a.mSelector) {
        case kAudioObjectPropertyBaseClass:
        case kAudioObjectPropertyClass:
        case kAudioObjectPropertyOwner:
        case kAudioObjectPropertyName:
        case kAudioObjectPropertyModelName:
        case kAudioObjectPropertyElementCategoryName:
        case kAudioObjectPropertyManufacturer:
            return true;
        case kAudioObjectPropertyOwnedObjects:
            return object != NS6AudioDriver::kOutputStreamObjectID &&
                   object != NS6AudioDriver::kInputStreamObjectID;
        case kAudioPlugInPropertyDeviceList:
        case kAudioPlugInPropertyBoxList:
        case kAudioPlugInPropertyClockDeviceList: return object == kPluginObjectID;
        case kAudioPlugInPropertyBundleID:
        case kAudioPlugInPropertyTranslateUIDToDevice:
            return object == kPluginObjectID;
        case kAudioPlugInPropertyResourceBundle: return object == kPluginObjectID;
        case kAudioObjectPropertyCustomPropertyInfoList: return true;
        case kAudioObjectPropertyControlList:
            return validDevice(object);
        case kAudioDevicePropertyDeviceUID:
        case kAudioDevicePropertyModelUID:
        case kAudioDevicePropertyTransportType:
        case kAudioDevicePropertyRelatedDevices:
        case kAudioDevicePropertyClockDomain:
        case kAudioDevicePropertyDeviceIsAlive:
        case kAudioDevicePropertyDeviceIsRunning:
        case kAudioDevicePropertyDeviceCanBeDefaultDevice:
        case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
        case kAudioDevicePropertyIsHidden:
        case kAudioDevicePropertyClockAlgorithm:
        case kAudioDevicePropertyClockIsStable:
        case kAudioDevicePropertyNominalSampleRate:
        case kAudioDevicePropertyAvailableNominalSampleRates:
        case kAudioDevicePropertyStreams:
        case kAudioDevicePropertyLatency:
        case kAudioDevicePropertySafetyOffset:
        case kAudioDevicePropertyZeroTimeStampPeriod:
        case kAudioDevicePropertyBufferFrameSize:
        case kAudioDevicePropertyBufferFrameSizeRange:
            return validDevice(object) ||
                   (a.mSelector == kAudioDevicePropertyLatency &&
                    (object == NS6AudioDriver::kInputStreamObjectID ||
                     object == NS6AudioDriver::kOutputStreamObjectID));
        case kAudioDevicePropertyPreferredChannelLayout:
            return (object == NS6AudioDriver::kDeviceObjectID &&
                    a.mScope == kAudioObjectPropertyScopeOutput) ||
                   (object == NS6AudioDriver::kInputDeviceObjectID &&
                    a.mScope == kAudioObjectPropertyScopeInput);
        case kAudioStreamPropertyIsActive:
        case kAudioStreamPropertyDirection:
        case kAudioStreamPropertyTerminalType:
        case kAudioStreamPropertyStartingChannel:
        case kAudioStreamPropertyVirtualFormat:
        case kAudioStreamPropertyPhysicalFormat:
        case kAudioStreamPropertyAvailableVirtualFormats:
        case kAudioStreamPropertyAvailablePhysicalFormats:
            return object == NS6AudioDriver::kOutputStreamObjectID ||
                   object == NS6AudioDriver::kInputStreamObjectID;
        default: return false;
    }
}

Boolean STDMETHODCALLTYPE hasPropertyCallback(AudioServerPlugInDriverRef, AudioObjectID object,
                                                pid_t, const AudioObjectPropertyAddress* address) {
    return address != nullptr && hasProperty(object, *address);
}
OSStatus STDMETHODCALLTYPE isSettable(AudioServerPlugInDriverRef, AudioObjectID object, pid_t,
                                       const AudioObjectPropertyAddress* a, Boolean* out) {
    if (a == nullptr || out == nullptr || !hasProperty(object, *a))
        return kAudioHardwareUnknownPropertyError;
    *out = false;
    return noErr;
}

AudioStreamBasicDescription streamFormat(bool input) {
    const UInt32 channels = input ? NS6AudioDriver::kInputChannels : NS6AudioDriver::kChannels;
    const UInt32 bytesPerFrame = channels * sizeof(Float32);
    // CoreAudio exchanges Float32; the USB worker converts to/from the device's 24-bit PCM.
    return {static_cast<Float64>(NS6AudioDriver::kSampleRate), kAudioFormatLinearPCM,
            kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked,
            bytesPerFrame, 1, bytesPerFrame, channels, 32, 0};
}

OSStatus propertyData(AudioObjectID object, const AudioObjectPropertyAddress& a,
                      UInt32 qualifierSize, const void* qualifierData,
                      UInt32 capacity, UInt32* size, void* data) {
    if (size == nullptr || !hasProperty(object, a))
        return kAudioHardwareUnknownPropertyError;
    if (a.mSelector == kAudioObjectPropertyCustomPropertyInfoList ||
        a.mSelector == kAudioObjectPropertyControlList ||
        a.mSelector == kAudioPlugInPropertyBoxList ||
        a.mSelector == kAudioPlugInPropertyClockDeviceList) {
        *size = 0;
        return noErr;
    }
    if (a.mSelector == kAudioDevicePropertyStreams &&
        ((object == NS6AudioDriver::kDeviceObjectID && a.mScope != kAudioObjectPropertyScopeOutput) ||
         (object == NS6AudioDriver::kInputDeviceObjectID && a.mScope != kAudioObjectPropertyScopeInput)))
        { *size = 0; return noErr; }
    if (data == nullptr) return kAudioHardwareIllegalOperationError;
    auto copy = [&](const void* source, UInt32 bytes) -> OSStatus {
        if (capacity < bytes) return kAudioHardwareBadPropertySizeError;
        std::memcpy(data, source, bytes); *size = bytes; return noErr;
    };
    switch (a.mSelector) {
        case kAudioObjectPropertyBaseClass: {
            const AudioClassID value = object == kPluginObjectID ? kAudioObjectClassID :
                validDevice(object) ? kAudioDeviceClassID : kAudioStreamClassID;
            return copy(&value, sizeof(value));
        }
        case kAudioObjectPropertyClass: {
            const AudioClassID value = object == kPluginObjectID ? kAudioPlugInClassID :
                validDevice(object) ? kAudioDeviceClassID : kAudioStreamClassID;
            return copy(&value, sizeof(value));
        }
        case kAudioObjectPropertyOwner: {
            const AudioObjectID value = object == kAudioObjectPlugInObject ? kAudioObjectUnknown :
                validDevice(object) ? kPluginObjectID :
                object == NS6AudioDriver::kInputStreamObjectID ? NS6AudioDriver::kInputDeviceObjectID
                                                               : NS6AudioDriver::kDeviceObjectID;
            return copy(&value, sizeof(value));
        }
        case kAudioObjectPropertyName:
        case kAudioObjectPropertyModelName:
        case kAudioObjectPropertyElementCategoryName:
        case kAudioObjectPropertyManufacturer: {
            CFStringRef value = a.mSelector == kAudioObjectPropertyManufacturer
                ? CFSTR("Numark") : a.mSelector == kAudioObjectPropertyModelName
                ? CFSTR("Numark NS6") : a.mSelector == kAudioObjectPropertyElementCategoryName
                    ? CFSTR("Audio") : object == NS6AudioDriver::kInputStreamObjectID
                        ? CFSTR("Numark NS6 In Stream")
                    : object == NS6AudioDriver::kOutputStreamObjectID
                        ? CFSTR("Numark NS6 Out Stream") : object == NS6AudioDriver::kDeviceObjectID
                            ? CFSTR("Numark NS6 Output") : object == NS6AudioDriver::kInputDeviceObjectID
                            ? CFSTR("Numark NS6 Input") : CFSTR("Numark NS6 Audio Driver");
            if (capacity < sizeof(value)) return kAudioHardwareBadPropertySizeError;
            CFRetain(value); std::memcpy(data, &value, sizeof(value)); *size = sizeof(value); return noErr;
        }
        case kAudioPlugInPropertyDeviceList: {
            const AudioObjectID values[]{NS6AudioDriver::kDeviceObjectID,
                                         NS6AudioDriver::kInputDeviceObjectID};
            return copy(values, sizeof(values));
        }
        case kAudioPlugInPropertyBundleID: {
            if (capacity < sizeof(kPluginBundleID)) return kAudioHardwareBadPropertySizeError;
            CFRetain(kPluginBundleID);
            std::memcpy(data, &kPluginBundleID, sizeof(kPluginBundleID));
            *size = sizeof(kPluginBundleID);
            return noErr;
        }
        case kAudioPlugInPropertyTranslateUIDToDevice: {
            AudioObjectID value = kAudioObjectUnknown;
            if (qualifierSize == sizeof(CFStringRef) && qualifierData != nullptr) {
                const auto uid = *static_cast<const CFStringRef*>(qualifierData);
                if (uid != nullptr && CFEqual(uid, kDeviceUID))
                    value = NS6AudioDriver::kDeviceObjectID;
                else if (uid != nullptr && CFEqual(uid, kInputDeviceUID))
                    value = NS6AudioDriver::kInputDeviceObjectID;
            }
            return copy(&value, sizeof(value));
        }
        case kAudioObjectPropertyOwnedObjects: {
            if (object == NS6AudioDriver::kOutputStreamObjectID ||
                object == NS6AudioDriver::kInputStreamObjectID) { *size = 0; return noErr; }
            if (object == kAudioObjectPlugInObject) {
                const AudioObjectID values[]{NS6AudioDriver::kDeviceObjectID,
                                             NS6AudioDriver::kInputDeviceObjectID};
                return copy(values, sizeof(values));
            }
            const AudioObjectID value = object == NS6AudioDriver::kDeviceObjectID
                ? NS6AudioDriver::kOutputStreamObjectID : NS6AudioDriver::kInputStreamObjectID;
            return copy(&value, sizeof(value));
        }
        case kAudioPlugInPropertyResourceBundle: {
            CFStringRef value = CFSTR("");
            if (capacity < sizeof(value)) return kAudioHardwareBadPropertySizeError;
            CFRetain(value); std::memcpy(data, &value, sizeof(value)); *size = sizeof(value); return noErr;
        }
        case kAudioDevicePropertyDeviceUID:
        case kAudioDevicePropertyModelUID: {
            CFStringRef value = a.mSelector == kAudioDevicePropertyModelUID ? kModelUID
                : object == NS6AudioDriver::kInputDeviceObjectID ? kInputDeviceUID : kDeviceUID;
            if (capacity < sizeof(value)) return kAudioHardwareBadPropertySizeError;
            CFRetain(value); std::memcpy(data, &value, sizeof(value)); *size = sizeof(value); return noErr;
        }
        case kAudioDevicePropertyTransportType: { const UInt32 v = kAudioDeviceTransportTypeUSB; return copy(&v, sizeof(v)); }
        case kAudioDevicePropertyRelatedDevices: {
            return copy(&object, sizeof(object));
        }
        case kAudioDevicePropertyClockDomain: { const UInt32 v = 0; return copy(&v, sizeof(v)); }
        case kAudioDevicePropertyDeviceIsAlive: { const UInt32 v = 1; return copy(&v, sizeof(v)); }
        case kAudioDevicePropertyDeviceIsRunning: {
            const UInt32 v = lifecycle(object).activeStarts() != 0; return copy(&v, sizeof(v));
        }
        case kAudioDevicePropertyDeviceCanBeDefaultDevice: { const UInt32 v = 1; return copy(&v, sizeof(v)); }
        case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice: {
            const UInt32 v = object == NS6AudioDriver::kDeviceObjectID; return copy(&v, sizeof(v));
        }
        case kAudioDevicePropertyIsHidden: { const UInt32 v = 0; return copy(&v, sizeof(v)); }
        case kAudioDevicePropertyClockAlgorithm: {
            const UInt32 v = kAudioDeviceClockAlgorithmSimpleIIR;
            return copy(&v, sizeof(v));
        }
        case kAudioDevicePropertyClockIsStable: { const UInt32 v = 1; return copy(&v, sizeof(v)); }
        case kAudioDevicePropertyNominalSampleRate: { const Float64 v = NS6AudioDriver::kSampleRate; return copy(&v, sizeof(v)); }
        case kAudioDevicePropertyAvailableNominalSampleRates: {
            const AudioValueRange v{static_cast<Float64>(NS6AudioDriver::kSampleRate), static_cast<Float64>(NS6AudioDriver::kSampleRate)};
            return copy(&v, sizeof(v));
        }
        case kAudioDevicePropertyStreams: {
            const AudioObjectID v = object == NS6AudioDriver::kInputDeviceObjectID
                ? NS6AudioDriver::kInputStreamObjectID : NS6AudioDriver::kOutputStreamObjectID;
            return copy(&v, sizeof(v));
        }
        case kAudioDevicePropertyLatency: {
            const UInt32 v = kDeviceLatencyFrames;
            return copy(&v, sizeof(v));
        }
        case kAudioDevicePropertySafetyOffset: { const UInt32 v = kIOBufferFrames; return copy(&v, sizeof(v)); }
        case kAudioDevicePropertyPreferredChannelLayout: {
            AudioChannelLayout v{};
            v.mChannelLayoutTag = a.mScope == kAudioObjectPropertyScopeInput
                ? kAudioChannelLayoutTag_Stereo : kAudioChannelLayoutTag_DiscreteInOrder;
            return copy(&v, offsetof(AudioChannelLayout, mChannelDescriptions));
        }
        case kAudioDevicePropertyZeroTimeStampPeriod: { const UInt32 v = kIOBufferFrames; return copy(&v, sizeof(v)); }
        case kAudioDevicePropertyBufferFrameSize: { const UInt32 v = kIOBufferFrames; return copy(&v, sizeof(v)); }
        case kAudioDevicePropertyBufferFrameSizeRange: { const AudioValueRange v{ kIOBufferFrames, kIOBufferFrames }; return copy(&v, sizeof(v)); }
        case kAudioStreamPropertyIsActive: { const UInt32 v = 1; return copy(&v, sizeof(v)); }
        case kAudioStreamPropertyDirection: {
            const UInt32 v = object == NS6AudioDriver::kInputStreamObjectID ? 1 : 0;
            return copy(&v, sizeof(v));
        }
        case kAudioStreamPropertyTerminalType: {
            const UInt32 v = object == NS6AudioDriver::kInputStreamObjectID
                ? kAudioStreamTerminalTypeMicrophone : kAudioStreamTerminalTypeSpeaker;
            return copy(&v, sizeof(v));
        }
        case kAudioStreamPropertyStartingChannel: { const UInt32 v = 1; return copy(&v, sizeof(v)); }
        case kAudioStreamPropertyVirtualFormat:
        case kAudioStreamPropertyPhysicalFormat: {
            const auto v = streamFormat(object == NS6AudioDriver::kInputStreamObjectID);
            return copy(&v, sizeof(v));
        }
        case kAudioStreamPropertyAvailableVirtualFormats:
        case kAudioStreamPropertyAvailablePhysicalFormats: {
            const AudioStreamRangedDescription v{
                streamFormat(object == NS6AudioDriver::kInputStreamObjectID),
                {static_cast<Float64>(NS6AudioDriver::kSampleRate), static_cast<Float64>(NS6AudioDriver::kSampleRate)}};
            return copy(&v, sizeof(v));
        }
        default: return kAudioHardwareUnknownPropertyError;
    }
}

OSStatus STDMETHODCALLTYPE getPropertySize(AudioServerPlugInDriverRef, AudioObjectID object, pid_t,
                                            const AudioObjectPropertyAddress* a, UInt32,
                                            const void*, UInt32* size) {
    if (a == nullptr || size == nullptr) return kAudioHardwareIllegalOperationError;
    if (!hasProperty(object, *a)) {
        os_log_error(OS_LOG_DEFAULT, "NS6 GetPropertyDataSize rejected object=%{public}u selector=%{public}u scope=%{public}u element=%{public}u",
                     object, a->mSelector, a->mScope, a->mElement);
        return kAudioHardwareUnknownPropertyError;
    }
    switch (a->mSelector) {
        case kAudioObjectPropertyName: case kAudioObjectPropertyModelName:
        case kAudioObjectPropertyElementCategoryName: case kAudioObjectPropertyManufacturer:
        case kAudioDevicePropertyDeviceUID: case kAudioDevicePropertyModelUID:
        case kAudioPlugInPropertyBundleID:
        case kAudioPlugInPropertyResourceBundle: *size = sizeof(CFStringRef); break;
        case kAudioObjectPropertyBaseClass: case kAudioObjectPropertyClass:
        case kAudioObjectPropertyOwner:
        case kAudioPlugInPropertyTranslateUIDToDevice:
        case kAudioDevicePropertyTransportType:
        case kAudioDevicePropertyClockDomain:
        case kAudioDevicePropertyDeviceIsAlive: case kAudioDevicePropertyDeviceIsRunning:
        case kAudioDevicePropertyDeviceCanBeDefaultDevice:
        case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
        case kAudioDevicePropertyIsHidden: case kAudioDevicePropertyClockAlgorithm:
        case kAudioDevicePropertyClockIsStable:
        case kAudioDevicePropertyLatency:
        case kAudioDevicePropertySafetyOffset: case kAudioDevicePropertyZeroTimeStampPeriod:
        case kAudioDevicePropertyBufferFrameSize: case kAudioStreamPropertyIsActive:
        case kAudioStreamPropertyDirection: case kAudioStreamPropertyTerminalType:
        case kAudioStreamPropertyStartingChannel: *size = sizeof(UInt32); break;
        case kAudioDevicePropertyNominalSampleRate: *size = sizeof(Float64); break;
        case kAudioDevicePropertyAvailableNominalSampleRates:
        case kAudioDevicePropertyBufferFrameSizeRange: *size = sizeof(AudioValueRange); break;
        case kAudioPlugInPropertyDeviceList: *size = 2 * sizeof(AudioObjectID); break;
        case kAudioDevicePropertyRelatedDevices: *size = sizeof(AudioObjectID); break;
        case kAudioObjectPropertyCustomPropertyInfoList:
        case kAudioObjectPropertyControlList:
        case kAudioPlugInPropertyBoxList:
        case kAudioPlugInPropertyClockDeviceList: *size = 0; break;
        case kAudioDevicePropertyStreams:
            *size = ((object == NS6AudioDriver::kDeviceObjectID &&
                      a->mScope == kAudioObjectPropertyScopeOutput) ||
                     (object == NS6AudioDriver::kInputDeviceObjectID &&
                      a->mScope == kAudioObjectPropertyScopeInput)) ? sizeof(AudioObjectID) : 0;
            break;
        case kAudioObjectPropertyOwnedObjects:
            *size = object == kPluginObjectID ? 2 * sizeof(AudioObjectID) : sizeof(AudioObjectID);
            break;
        case kAudioDevicePropertyPreferredChannelLayout:
            *size = offsetof(AudioChannelLayout, mChannelDescriptions);
            break;
        case kAudioStreamPropertyVirtualFormat: case kAudioStreamPropertyPhysicalFormat: *size = sizeof(AudioStreamBasicDescription); break;
        case kAudioStreamPropertyAvailableVirtualFormats: case kAudioStreamPropertyAvailablePhysicalFormats: *size = sizeof(AudioStreamRangedDescription); break;
        default: return kAudioHardwareUnknownPropertyError;
    }
    return noErr;
}

OSStatus STDMETHODCALLTYPE getProperty(AudioServerPlugInDriverRef, AudioObjectID object, pid_t,
                                        const AudioObjectPropertyAddress* a, UInt32 qSize,
                                        const void* qData, UInt32 capacity, UInt32* size, void* data) {
    if (a == nullptr) return kAudioHardwareIllegalOperationError;
    const OSStatus result = propertyData(object, *a, qSize, qData, capacity, size, data);
    if (result != noErr)
        os_log_error(OS_LOG_DEFAULT, "NS6 GetPropertyData failed object=%{public}u selector=%{public}u scope=%{public}u status=%{public}d",
                     object, a->mSelector, a->mScope, result);
    return result;
}
OSStatus STDMETHODCALLTYPE setProperty(AudioServerPlugInDriverRef, AudioObjectID, pid_t,
                                        const AudioObjectPropertyAddress*, UInt32, const void*,
                                        UInt32, const void*) {
    return kAudioHardwareUnsupportedOperationError;
}

IOReturn startHardware() {
    try {
        gState.session = std::make_unique<NS6DeviceSession>();
        IOReturn result = gState.session->open();
        if (result != kIOReturnSuccess) {
            os_log_error(OS_LOG_DEFAULT, "NS6 StartIO stage=open status=%{public}d", result);
            gState.session.reset(); return result;
        }
        result = gState.session->configureAudioInputMode();
        if (result != kIOReturnSuccess) {
            os_log_error(OS_LOG_DEFAULT, "NS6 StartIO stage=input-mode status=%{public}d", result);
            gState.session->close(); gState.session.reset(); return result;
        }
        gState.runtime = std::make_unique<AudioTransportRuntime>(*gState.session,
                                                                 gState.output.ring(),
                                                                 gState.input);
        result = gState.runtime->start();
        if (result != kIOReturnSuccess)
            os_log_error(OS_LOG_DEFAULT, "NS6 StartIO stage=runtime-first-start status=%{public}d", result);
        if (result == kIOReturnSuccess) {
            result = gState.runtime->stop();
            if (result != kIOReturnSuccess)
                os_log_error(OS_LOG_DEFAULT, "NS6 StartIO stage=runtime-stop-before-activation status=%{public}d", result);
        }
        bool replySeen = false;
        if (result == kIOReturnSuccess) {
            result = ControllerActivation::activate(*gState.session, &replySeen);
            if (result != kIOReturnSuccess)
                os_log_error(OS_LOG_DEFAULT, "NS6 StartIO stage=activation status=%{public}d reply=%{public}d", result, replySeen);
        }
        if (result == kIOReturnSuccess) {
            result = gState.session->configureAudioInputMode();
            if (result != kIOReturnSuccess)
                os_log_error(OS_LOG_DEFAULT, "NS6 StartIO stage=input-mode-after-activation status=%{public}d", result);
        }
        if (result == kIOReturnSuccess) {
            result = gState.runtime->start();
            if (result != kIOReturnSuccess)
                os_log_error(OS_LOG_DEFAULT, "NS6 StartIO stage=runtime-after-activation status=%{public}d", result);
        }
        if (result != kIOReturnSuccess) {
            gState.runtime->stop(); gState.runtime.reset(); gState.session->close(); gState.session.reset();
            return result;
        }
        gState.running.store(true);
        return kIOReturnSuccess;
    } catch (const std::bad_alloc&) {
        if (gState.runtime) gState.runtime->stop();
        gState.runtime.reset();
        if (gState.session) gState.session->close();
        gState.session.reset();
        return kIOReturnNoMemory;
    }
}

IOReturn stopHardware() {
    IOReturn result = gState.runtime ? gState.runtime->stop() : kIOReturnSuccess;
    if (result != kIOReturnSuccess) return result;
    gState.runtime.reset();
    result = gState.session ? gState.session->close() : kIOReturnSuccess;
    if (result == kIOReturnSuccess) {
        gState.session.reset();
        gState.running.store(false);
    }
    return result;
}

OSStatus STDMETHODCALLTYPE startIO(AudioServerPlugInDriverRef, AudioObjectID device, UInt32 client) {
    if (!validDevice(device)) return kAudioHardwareBadObjectError;
    std::unique_lock<std::mutex> lock(gState.mutex);
    if (!hasClient(device, client)) return kAudioHardwareBadObjectError;
    const bool wasIdle = !anyActive();
    const auto transition = lifecycle(device).startIO(client);
    if (transition == AudioIOTransition::Rejected) return kAudioHardwareBadObjectError;
    if (wasIdle) {
        const IOReturn result = startHardware();
        if (result != kIOReturnSuccess) {
            os_log_error(OS_LOG_DEFAULT, "NS6 StartIO failed device=%{public}u client=%{public}u status=%{public}d", device, client, result);
            lifecycle(device).stopIO(client);
            return result;
        }
    }
    os_log_info(OS_LOG_DEFAULT, "NS6 StartIO started device=%{public}u client=%{public}u", device, client);
    lock.unlock();
    if (transition == AudioIOTransition::FirstClientStarted) notifyRunningChanged(device);
    return noErr;
}
OSStatus STDMETHODCALLTYPE stopIO(AudioServerPlugInDriverRef, AudioObjectID device, UInt32 client) {
    if (!validDevice(device)) return kAudioHardwareBadObjectError;
    std::unique_lock<std::mutex> lock(gState.mutex);
    if (!hasClient(device, client)) return kAudioHardwareBadObjectError;
    const auto transition = lifecycle(device).stopIO(client);
    if (transition == AudioIOTransition::ClientNotActive) return noErr;
    if (transition == AudioIOTransition::LastClientStopped) {
        const IOReturn result = anyActive() ? kIOReturnSuccess : stopHardware();
        lock.unlock();
        if (result == kIOReturnSuccess) notifyRunningChanged(device);
        return result;
    }
    return noErr;
}
OSStatus STDMETHODCALLTYPE zeroTimestamp(AudioServerPlugInDriverRef, AudioObjectID device, UInt32,
                                          Float64* sample, UInt64* host, UInt64* seed) {
    if (!validDevice(device) || sample == nullptr || host == nullptr || seed == nullptr)
        return kAudioHardwareIllegalOperationError;
    const UInt64 now = mach_absolute_time();
    mach_timebase_info_data_t timebase{};
    mach_timebase_info(&timebase);
    const UInt64 period = ((static_cast<UInt64>(kIOBufferFrames) * 1000000000ULL) /
                           NS6AudioDriver::kSampleRate) * timebase.denom / timebase.numer;
    const UInt64 anchor = now - (now % period);
    *sample = static_cast<Float64>(anchor / period) * kIOBufferFrames;
    *host = anchor;
    *seed = gState.timestampSeed;
    return noErr;
}
OSStatus STDMETHODCALLTYPE willDo(AudioServerPlugInDriverRef, AudioObjectID device, UInt32,
                                   UInt32 op, Boolean* yes, Boolean* inPlace) {
    if (yes == nullptr || inPlace == nullptr) return kAudioHardwareIllegalOperationError;
    *yes = (device == NS6AudioDriver::kDeviceObjectID &&
            op == kAudioServerPlugInIOOperationWriteMix) ||
           (device == NS6AudioDriver::kInputDeviceObjectID &&
            op == kAudioServerPlugInIOOperationReadInput);
    *inPlace = true;
    return noErr;
}
OSStatus STDMETHODCALLTYPE beginIO(AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32,
                                    UInt32, const AudioServerPlugInIOCycleInfo*) { return noErr; }
OSStatus STDMETHODCALLTYPE doIO(AudioServerPlugInDriverRef, AudioObjectID device, AudioObjectID stream,
                                 UInt32, UInt32 op, UInt32 frames,
                                 const AudioServerPlugInIOCycleInfo*, void* mainBuffer, void*) {
    if (device == NS6AudioDriver::kDeviceObjectID &&
        stream == NS6AudioDriver::kOutputStreamObjectID &&
        op == kAudioServerPlugInIOOperationWriteMix)
        return NS6AudioDriver::processOutputMix(gState.output, op, frames,
                                                 static_cast<const Float32*>(mainBuffer));
    if (device == NS6AudioDriver::kInputDeviceObjectID &&
        stream == NS6AudioDriver::kInputStreamObjectID &&
        op == kAudioServerPlugInIOOperationReadInput)
        return NS6AudioDriver::processInput(gState.input, op, frames,
                                             static_cast<Float32*>(mainBuffer));
    return noErr;
}
OSStatus STDMETHODCALLTYPE endIO(AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32,
                                  UInt32, const AudioServerPlugInIOCycleInfo*) { return noErr; }

__attribute__((constructor)) void initializeInterface() {
    gInterface = {nullptr, queryInterface, addRef, release, initialize, createDevice, destroyDevice,
        addClient, removeClient, performConfiguration, abortConfiguration, hasPropertyCallback,
        isSettable, getPropertySize, getProperty, setProperty, startIO, stopIO, zeroTimestamp,
        willDo, beginIO, doIO, endIO};
}
}

extern "C" void* NS6AudioDriverFactory(CFAllocatorRef, CFUUIDRef type) {
    const bool matches = type != nullptr && CFEqual(type, kAudioServerPlugInTypeUUID);
    os_log_info(OS_LOG_DEFAULT, "NS6 Audio factory type matched=%{public}d", matches);
    return matches ? gDriver : nullptr;
}
