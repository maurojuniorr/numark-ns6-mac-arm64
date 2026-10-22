#include "../coreaudio/NS6AudioDriver.hpp"
#include "../coreaudio/AudioOutputEngine.hpp"
#include "../coreaudio/AudioInputEngine.hpp"

#include <array>
#include <cassert>

extern "C" void* NS6AudioDriverFactory(CFAllocatorRef allocator, CFUUIDRef type);

namespace {
AudioObjectID gChangedObject = kAudioObjectUnknown;
AudioObjectPropertySelector gChangedSelector = 0;

OSStatus capturePropertiesChanged(AudioServerPlugInHostRef, AudioObjectID object,
                                  UInt32 count, const AudioObjectPropertyAddress* addresses) {
    assert(count == 1 && addresses != nullptr);
    gChangedObject = object;
    gChangedSelector = addresses[0].mSelector;
    return noErr;
}

AudioStreamBasicDescription getStreamFormat(AudioServerPlugInDriverRef driver,
                                             AudioObjectID stream,
                                             AudioObjectPropertySelector selector) {
    const AudioObjectPropertyAddress address{selector, kAudioObjectPropertyScopeGlobal,
                                               kAudioObjectPropertyElementMain};
    UInt32 size = 0;
    assert((*driver)->GetPropertyDataSize(driver, stream, 0,
                                          &address, 0, nullptr, &size) == noErr);
    assert(size == sizeof(AudioStreamBasicDescription));
    AudioStreamBasicDescription format{};
    assert((*driver)->GetPropertyData(driver, stream, 0,
                                      &address, 0, nullptr, size, &size, &format) == noErr);
    return format;
}
}

int main() {
    static_assert(NS6AudioDriver::kSampleRate == 44100);
    static_assert(NS6AudioDriver::kChannels == 4);
    static_assert(NS6AudioDriver::kBytesPerFrame == 16);

    auto driver = static_cast<AudioServerPlugInDriverRef>(
        NS6AudioDriverFactory(kCFAllocatorDefault, kAudioServerPlugInTypeUUID));
    assert(driver != nullptr);
    AudioServerPlugInHostInterface host{capturePropertiesChanged, nullptr, nullptr, nullptr, nullptr};
    assert((*driver)->Initialize(driver, &host) == noErr);
    assert(gChangedObject == kAudioObjectPlugInObject);
    assert(gChangedSelector == kAudioPlugInPropertyDeviceList);
    const AudioObjectPropertyAddress deviceList{kAudioPlugInPropertyDeviceList,
        kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    UInt32 propertySize = 0;
    assert((*driver)->GetPropertyDataSize(driver, kAudioObjectPlugInObject, 0,
        &deviceList, 0, nullptr, &propertySize) == noErr);
    assert(propertySize == 2 * sizeof(AudioObjectID));
    AudioObjectID devices[2]{};
    assert((*driver)->GetPropertyData(driver, kAudioObjectPlugInObject, 0,
        &deviceList, 0, nullptr, sizeof(devices), &propertySize, devices) == noErr);
    assert(devices[0] == NS6AudioDriver::kDeviceObjectID);
    assert(devices[1] == NS6AudioDriver::kInputDeviceObjectID);
    const AudioObjectPropertyAddress inputOwner{kAudioObjectPropertyOwner,
        kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    AudioObjectID owner = kAudioObjectUnknown;
    assert((*driver)->GetPropertyData(driver, NS6AudioDriver::kInputStreamObjectID, 0,
        &inputOwner, 0, nullptr, sizeof(owner), &propertySize, &owner) == noErr);
    assert(owner == NS6AudioDriver::kInputDeviceObjectID);
    AudioObjectID deviceID = kAudioObjectUnknown;

    const AudioObjectPropertyAddress bundleIDProperty{kAudioPlugInPropertyBundleID,
        kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    assert((*driver)->HasProperty(driver, kAudioObjectPlugInObject, 0, &bundleIDProperty));
    CFStringRef bundleID = nullptr;
    assert((*driver)->GetPropertyDataSize(driver, kAudioObjectPlugInObject, 0,
        &bundleIDProperty, 0, nullptr, &propertySize) == noErr);
    assert((*driver)->GetPropertyData(driver, kAudioObjectPlugInObject, 0,
        &bundleIDProperty, 0, nullptr, sizeof(bundleID), &propertySize, &bundleID) == noErr);
    assert(CFEqual(bundleID, CFSTR("io.github.maurojuniorr.NumarkNS6.AudioServer")));
    CFRelease(bundleID);
    const AudioObjectPropertyAddress modelUID{kAudioDevicePropertyModelUID,
        kAudioObjectPropertyScopeGlobal, UINT32_MAX};
    assert((*driver)->HasProperty(driver, NS6AudioDriver::kDeviceObjectID, 0, &modelUID));
    CFStringRef modelUIDValue = nullptr;
    assert((*driver)->GetPropertyDataSize(driver, NS6AudioDriver::kDeviceObjectID, 0,
        &modelUID, 0, nullptr, &propertySize) == noErr);
    assert((*driver)->GetPropertyData(driver, NS6AudioDriver::kDeviceObjectID, 0,
        &modelUID, 0, nullptr, sizeof(modelUIDValue), &propertySize, &modelUIDValue) == noErr);
    assert(CFEqual(modelUIDValue, CFSTR("io.github.maurojuniorr.numark-ns6")));
    CFRelease(modelUIDValue);
    const AudioObjectPropertyAddress translateUID{kAudioPlugInPropertyTranslateUIDToDevice,
        kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    assert((*driver)->HasProperty(driver, kAudioObjectPlugInObject, 0, &translateUID));
    CFStringRef deviceUID = CFSTR("io.github.maurojuniorr.numark-ns6.out");
    assert((*driver)->GetPropertyData(driver, kAudioObjectPlugInObject, 0,
        &translateUID, sizeof(deviceUID), &deviceUID, sizeof(deviceID), &propertySize,
        &deviceID) == noErr);
    assert(deviceID == NS6AudioDriver::kDeviceObjectID);
    CFStringRef inputUID = CFSTR("io.github.maurojuniorr.numark-ns6.in");
    assert((*driver)->GetPropertyData(driver, kAudioObjectPlugInObject, 0,
        &translateUID, sizeof(inputUID), &inputUID, sizeof(deviceID), &propertySize,
        &deviceID) == noErr);
    assert(deviceID == NS6AudioDriver::kInputDeviceObjectID);
    CFStringRef unknownUID = CFSTR("io.github.maurojuniorr.unknown-device");
    deviceID = NS6AudioDriver::kDeviceObjectID;
    assert((*driver)->GetPropertyData(driver, kAudioObjectPlugInObject, 0,
        &translateUID, sizeof(unknownUID), &unknownUID, sizeof(deviceID), &propertySize,
        &deviceID) == noErr);
    assert(deviceID == kAudioObjectUnknown);

    for (const AudioObjectPropertySelector selector :
         std::array<AudioObjectPropertySelector, 2>{kAudioObjectPropertyModelName,
             kAudioObjectPropertyElementCategoryName}) {
        const AudioObjectPropertyAddress property{selector, kAudioObjectPropertyScopeGlobal,
                                                   kAudioObjectPropertyElementMain};
        assert((*driver)->HasProperty(driver, NS6AudioDriver::kDeviceObjectID, 0, &property));
        CFStringRef value = nullptr;
        assert((*driver)->GetPropertyData(driver, NS6AudioDriver::kDeviceObjectID, 0,
            &property, 0, nullptr, sizeof(value), &propertySize, &value) == noErr);
        assert(value != nullptr);
        CFRelease(value);
    }

    for (const auto [object, selector] : {
             std::pair<AudioObjectID, AudioObjectPropertySelector>{kAudioObjectPlugInObject, kAudioPlugInPropertyBoxList},
             {kAudioObjectPlugInObject, kAudioPlugInPropertyClockDeviceList},
             {kAudioObjectPlugInObject, kAudioObjectPropertyCustomPropertyInfoList},
             {NS6AudioDriver::kDeviceObjectID, kAudioObjectPropertyControlList},
             {NS6AudioDriver::kDeviceObjectID, kAudioObjectPropertyCustomPropertyInfoList},
             {NS6AudioDriver::kOutputStreamObjectID, kAudioObjectPropertyCustomPropertyInfoList}}) {
        const AudioObjectPropertyAddress emptyList{selector, kAudioObjectPropertyScopeGlobal,
                                                    kAudioObjectPropertyElementMain};
        assert((*driver)->HasProperty(driver, object, 0, &emptyList));
        assert((*driver)->GetPropertyDataSize(driver, object, 0, &emptyList, 0, nullptr,
                                              &propertySize) == noErr);
        assert(propertySize == 0);
        AudioObjectID noItems = kAudioObjectUnknown;
        assert((*driver)->GetPropertyData(driver, object, 0, &emptyList, 0, nullptr,
                                          sizeof(noItems), &propertySize, &noItems) == noErr);
        assert(propertySize == 0);
    }

    for (const AudioObjectPropertySelector selector :
         std::array<AudioObjectPropertySelector, 3>{kAudioDevicePropertyClockAlgorithm,
             kAudioDevicePropertyClockIsStable, kAudioDevicePropertyIsHidden}) {
        const AudioObjectPropertyAddress property{selector, kAudioObjectPropertyScopeGlobal,
                                                   kAudioObjectPropertyElementMain};
        assert((*driver)->HasProperty(driver, NS6AudioDriver::kDeviceObjectID, 0, &property));
        UInt32 value = 0;
        assert((*driver)->GetPropertyData(driver, NS6AudioDriver::kDeviceObjectID, 0,
            &property, 0, nullptr, sizeof(value), &propertySize, &value) == noErr);
        if (selector == kAudioDevicePropertyClockAlgorithm)
            assert(value == kAudioDeviceClockAlgorithmSimpleIIR);
        else if (selector == kAudioDevicePropertyClockIsStable)
            assert(value == 1);
        else
            assert(value == 0);
    }
    for (const AudioObjectPropertySelector selector :
         std::array<AudioObjectPropertySelector, 2>{kAudioDevicePropertyDeviceCanBeDefaultDevice,
             kAudioDevicePropertyDeviceCanBeDefaultSystemDevice}) {
        const AudioObjectPropertyAddress property{selector, kAudioObjectPropertyScopeGlobal,
                                                   kAudioObjectPropertyElementMain};
        UInt32 value = 0;
        assert((*driver)->GetPropertyData(driver, NS6AudioDriver::kDeviceObjectID, 0,
            &property, 0, nullptr, sizeof(value), &propertySize, &value) == noErr);
        assert(value == 1);
    }
    const AudioObjectPropertyAddress inputDefault{kAudioDevicePropertyDeviceCanBeDefaultDevice,
        kAudioObjectPropertyScopeInput, kAudioObjectPropertyElementMain};
    UInt32 inputCanBeDefault = 0;
    assert((*driver)->GetPropertyData(driver, NS6AudioDriver::kInputDeviceObjectID, 0,
        &inputDefault, 0, nullptr, sizeof(inputCanBeDefault), &propertySize,
        &inputCanBeDefault) == noErr);
    assert(inputCanBeDefault == 1);

    const auto virtualFormat = getStreamFormat(driver, NS6AudioDriver::kOutputStreamObjectID,
                                                kAudioStreamPropertyVirtualFormat);
    assert(virtualFormat.mSampleRate == 44100);
    assert(virtualFormat.mChannelsPerFrame == 4);
    assert(virtualFormat.mBitsPerChannel == 32);
    assert(virtualFormat.mBytesPerFrame == 16);
    const auto physicalFormat = getStreamFormat(driver, NS6AudioDriver::kOutputStreamObjectID,
                                                 kAudioStreamPropertyPhysicalFormat);
    assert(physicalFormat.mSampleRate == 44100);
    assert(physicalFormat.mChannelsPerFrame == 4);
    assert(physicalFormat.mBitsPerChannel == 32);
    assert(physicalFormat.mBytesPerFrame == 16);
    assert((physicalFormat.mFormatFlags & kAudioFormatFlagIsFloat) != 0);
    const AudioObjectPropertyAddress inputStreams{kAudioDevicePropertyStreams,
        kAudioObjectPropertyScopeInput, kAudioObjectPropertyElementMain};
    assert((*driver)->HasProperty(driver, NS6AudioDriver::kInputDeviceObjectID, 0, &inputStreams));
    assert((*driver)->GetPropertyDataSize(driver, NS6AudioDriver::kInputDeviceObjectID, 0,
        &inputStreams, 0, nullptr, &propertySize) == noErr);
    assert(propertySize == sizeof(AudioObjectID));
    AudioObjectID inputStream = kAudioObjectUnknown;
    assert((*driver)->GetPropertyData(driver, NS6AudioDriver::kInputDeviceObjectID, 0,
        &inputStreams, 0, nullptr, sizeof(inputStream), &propertySize, &inputStream) == noErr);
    assert(inputStream == NS6AudioDriver::kInputStreamObjectID);
    assert((*driver)->GetPropertyDataSize(driver, NS6AudioDriver::kDeviceObjectID, 0,
        &inputStreams, 0, nullptr, &propertySize) == noErr);
    assert(propertySize == 0);
    AudioObjectID noInputStream = kAudioObjectUnknown;
    assert((*driver)->GetPropertyData(driver, NS6AudioDriver::kDeviceObjectID, 0,
        &inputStreams, 0, nullptr, sizeof(noInputStream), &propertySize, &noInputStream) == noErr);
    assert(propertySize == 0);
    const AudioObjectPropertyAddress outputStreams{kAudioDevicePropertyStreams,
        kAudioObjectPropertyScopeOutput, kAudioObjectPropertyElementMain};
    assert((*driver)->GetPropertyDataSize(driver, NS6AudioDriver::kInputDeviceObjectID, 0,
        &outputStreams, 0, nullptr, &propertySize) == noErr);
    assert(propertySize == 0);
    Boolean willDo = false;
    Boolean inPlace = false;
    assert((*driver)->WillDoIOOperation(driver, NS6AudioDriver::kDeviceObjectID, 1,
        kAudioServerPlugInIOOperationReadInput, &willDo, &inPlace) == noErr && !willDo);
    assert((*driver)->WillDoIOOperation(driver, NS6AudioDriver::kInputDeviceObjectID, 1,
        kAudioServerPlugInIOOperationReadInput, &willDo, &inPlace) == noErr && willDo);
    assert((*driver)->WillDoIOOperation(driver, NS6AudioDriver::kInputDeviceObjectID, 1,
        kAudioServerPlugInIOOperationWriteMix, &willDo, &inPlace) == noErr && !willDo);
    const auto inputVirtual = getStreamFormat(driver, inputStream, kAudioStreamPropertyVirtualFormat);
    assert(inputVirtual.mChannelsPerFrame == 2 && inputVirtual.mBitsPerChannel == 32 &&
           inputVirtual.mBytesPerFrame == 8);
    const auto inputPhysical = getStreamFormat(driver, inputStream, kAudioStreamPropertyPhysicalFormat);
    assert(inputPhysical.mChannelsPerFrame == 2 && inputPhysical.mBitsPerChannel == 32 &&
           inputPhysical.mBytesPerFrame == 8 &&
           (inputPhysical.mFormatFlags & kAudioFormatFlagIsFloat) != 0);
    const AudioObjectPropertyAddress inputLayoutProperty{kAudioDevicePropertyPreferredChannelLayout,
        kAudioObjectPropertyScopeInput, kAudioObjectPropertyElementMain};
    const AudioObjectPropertyAddress outputLayoutProperty{kAudioDevicePropertyPreferredChannelLayout,
        kAudioObjectPropertyScopeOutput, kAudioObjectPropertyElementMain};
    AudioChannelLayout layout{};
    assert((*driver)->GetPropertyData(driver, NS6AudioDriver::kInputDeviceObjectID, 0,
        &inputLayoutProperty, 0, nullptr, sizeof(layout), &propertySize, &layout) == noErr);
    assert(layout.mChannelLayoutTag == kAudioChannelLayoutTag_Stereo);
    assert((*driver)->GetPropertyData(driver, NS6AudioDriver::kDeviceObjectID, 0,
        &outputLayoutProperty, 0, nullptr, sizeof(layout), &propertySize, &layout) == noErr);
    assert(layout.mChannelLayoutTag == kAudioChannelLayoutTag_DiscreteInOrder);
    const AudioObjectPropertyAddress streamLatency{kAudioStreamPropertyLatency,
        kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    UInt32 latency = 0;
    assert((*driver)->GetPropertyData(driver, inputStream, 0, &streamLatency, 0, nullptr,
        sizeof(latency), &propertySize, &latency) == noErr);
    assert(latency == 256);
    AudioServerPlugInIOCycleInfo cycleInfo{};
    std::array<Float32, 8> samples{0.1F, 0.2F, 0.3F, 0.4F, 0.5F, 0.6F, 0.7F, 0.8F};
    assert((*driver)->DoIOOperation(driver, NS6AudioDriver::kDeviceObjectID,
        NS6AudioDriver::kOutputStreamObjectID, 1, kAudioServerPlugInIOOperationWriteMix,
        2, &cycleInfo, samples.data(), nullptr) == noErr);
    std::array<Float32, 4> rawInput{1.0F, 1.0F, 1.0F, 1.0F};
    assert((*driver)->DoIOOperation(driver, NS6AudioDriver::kInputDeviceObjectID,
        NS6AudioDriver::kInputStreamObjectID, 1, kAudioServerPlugInIOOperationReadInput,
        2, &cycleInfo, rawInput.data(), nullptr) == noErr);
    assert((rawInput == std::array<Float32, 4>{0, 0, 0, 0}));
    (*driver)->Release(driver);

    AudioOutputEngine engine(8);
    assert(NS6AudioDriver::processOutputMix(engine, kAudioServerPlugInIOOperationWriteMix,
                                            2, samples.data()) == noErr);
    std::array<Float32, 8> captured{};
    assert(engine.ring().read(captured.data(), 2) == 2);
    assert(captured == samples);

    assert(NS6AudioDriver::processOutputMix(engine, kAudioServerPlugInIOOperationWriteMix,
                                            2, nullptr) == kAudioHardwareIllegalOperationError);
    assert(NS6AudioDriver::processOutputMix(engine, kAudioServerPlugInIOOperationProcessOutput,
                                            2, nullptr) == noErr);

    AudioInputEngine input(8, 1);
    std::array<Float32, 4> capturedInput{1.0F, 1.0F, 1.0F, 1.0F};
    assert(NS6AudioDriver::processInput(input, kAudioServerPlugInIOOperationReadInput,
                                        2, capturedInput.data()) == noErr);
    assert((capturedInput == std::array<Float32, 4>{0, 0, 0, 0}));
    assert(NS6AudioDriver::processInput(input, kAudioServerPlugInIOOperationReadInput,
                                        2, nullptr) == kAudioHardwareIllegalOperationError);
}
