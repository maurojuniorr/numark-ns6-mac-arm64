#include "NS6DeviceSession.hpp"

#include <IOKit/IOCFPlugIn.h>
#include <CoreFoundation/CFNumber.h>

#include <chrono>
#include <cstdint>
#include <thread>

namespace {

bool numberProperty(io_registry_entry_t entry, CFStringRef key, std::uint16_t& value) {
    CFTypeRef property = IORegistryEntryCreateCFProperty(entry, key, kCFAllocatorDefault, 0);
    if (property == nullptr || CFGetTypeID(property) != CFNumberGetTypeID()) {
        if (property != nullptr) CFRelease(property);
        return false;
    }
    SInt16 signedValue = 0;
    const bool valid = CFNumberGetValue(static_cast<CFNumberRef>(property), kCFNumberSInt16Type,
                                        &signedValue);
    CFRelease(property);
    if (valid) value = static_cast<std::uint16_t>(signedValue);
    return valid;
}

IOUSBInterfaceInterface** matchingInterface(io_iterator_t iterator, UInt8 wantedNumber,
                                            const std::uint64_t* wantedDeviceID,
                                            std::uint64_t* matchedDeviceID, IOReturn& result) {
    io_service_t service = IO_OBJECT_NULL;
    while ((service = IOIteratorNext(iterator)) != IO_OBJECT_NULL) {
        io_registry_entry_t device = IO_OBJECT_NULL;
        std::uint16_t vendor = 0;
        std::uint16_t product = 0;
        std::uint16_t speed = 0;
        std::uint64_t deviceID = 0;
        const bool hasParent =
                IORegistryEntryGetParentEntry(service, kIOServicePlane, &device) == KERN_SUCCESS;
        const bool hasVendor = hasParent && numberProperty(device, CFSTR(kUSBVendorID), vendor);
        const bool hasProduct = hasParent && numberProperty(device, CFSTR(kUSBProductID), product);
        const bool hasSpeed = hasParent && numberProperty(device, CFSTR(kUSBDevicePropertySpeed), speed);
        const bool hasDeviceID = hasParent && IORegistryEntryGetRegistryEntryID(device, &deviceID) == KERN_SUCCESS;
        const bool matchingDevice =
                hasParent && hasVendor && hasProduct && hasSpeed && hasDeviceID &&
                vendor == 0x15e4 && product == 0x0079 && speed == kUSBDeviceSpeedHigh &&
                (wantedDeviceID == nullptr || *wantedDeviceID == deviceID);
        if (device != IO_OBJECT_NULL) IOObjectRelease(device);
        if (!matchingDevice) {
            IOObjectRelease(service);
            continue;
        }

        IOCFPlugInInterface** plugin = nullptr;
        SInt32 score = 0;
        HRESULT status = IOCreatePlugInInterfaceForService(
                service, kIOUSBInterfaceUserClientTypeID, kIOCFPlugInInterfaceID, &plugin, &score);
        IOObjectRelease(service);
        if (status != kIOReturnSuccess || plugin == nullptr) continue;

        IOUSBInterfaceInterface** candidate = nullptr;
        status = (*plugin)->QueryInterface(plugin, CFUUIDGetUUIDBytes(kIOUSBInterfaceInterfaceID),
                                           reinterpret_cast<LPVOID*>(&candidate));
        IODestroyPlugInInterface(plugin);
        if (status != S_OK || candidate == nullptr) continue;

        UInt8 number = 0xff;
        status = (*candidate)->GetInterfaceNumber(candidate, &number);
        if (status == kIOReturnSuccess && number == wantedNumber) {
            if (matchedDeviceID != nullptr) *matchedDeviceID = deviceID;
            result = kIOReturnSuccess;
            return candidate;
        }
        (*candidate)->Release(candidate);
    }
    result = kIOReturnNotFound;
    return nullptr;
}

IOReturn configureDevice() {
    io_iterator_t iterator = IO_OBJECT_NULL;
    kern_return_t result = IOServiceGetMatchingServices(
            kIOMainPortDefault, IOServiceMatching("IOUSBHostDevice"), &iterator);
    if (result != KERN_SUCCESS) return result;

    io_service_t service = IO_OBJECT_NULL;
    while ((service = IOIteratorNext(iterator)) != IO_OBJECT_NULL) {
        std::uint16_t vendor = 0;
        std::uint16_t product = 0;
        if (!numberProperty(service, CFSTR(kUSBVendorID), vendor) ||
            !numberProperty(service, CFSTR(kUSBProductID), product) ||
            vendor != 0x15e4 || product != 0x0079) {
            IOObjectRelease(service);
            continue;
        }

        IOCFPlugInInterface** plugin = nullptr;
        IOUSBDeviceInterface** device = nullptr;
        SInt32 score = 0;
        result = IOCreatePlugInInterfaceForService(service, kIOUSBDeviceUserClientTypeID,
                                                   kIOCFPlugInInterfaceID, &plugin, &score);
        IOObjectRelease(service);
        IOObjectRelease(iterator);
        if (result == kIOReturnSuccess && plugin != nullptr) {
            result = (*plugin)->QueryInterface(plugin, CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID),
                                               reinterpret_cast<LPVOID*>(&device));
            IODestroyPlugInInterface(plugin);
        }
        if (result != kIOReturnSuccess || device == nullptr) return result;

        result = (*device)->USBDeviceOpenSeize(device);
        if (result == kIOReturnSuccess) {
            UInt8 configuration = 0;
            result = (*device)->GetConfiguration(device, &configuration);
            if (result == kIOReturnSuccess) result = (*device)->SetConfiguration(device, 1);
            (*device)->USBDeviceClose(device);
        }
        (*device)->Release(device);
        return result;
    }
    IOObjectRelease(iterator);
    return kIOReturnNotFound;
}

}  // namespace

NS6DeviceSession::~NS6DeviceSession() {
    close();
}

IOReturn NS6DeviceSession::configureAudioInputMode() noexcept {
    if (!opened_ || interface_ == nullptr) return kIOReturnNotReady;
    UInt8 capability[8]{};
    IOUSBDevRequest capabilityRead{0xc0, 86, 0, 0, sizeof(capability), capability, 0};
    IOReturn result = (*interface_)->ControlRequest(interface_, 0, &capabilityRead);
    if (result != kIOReturnSuccess) return result;
    const UInt16 capabilityLength = capability[0] < sizeof(capability)
                                            ? capability[0]
                                            : sizeof(capability);
    if (capabilityLength != 0) {
        capabilityRead.wLength = capabilityLength;
        capabilityRead.wLenDone = 0;
        result = (*interface_)->ControlRequest(interface_, 0, &capabilityRead);
        if (result != kIOReturnSuccess) return result;
    }

    UInt8 rate[3] = {0x44, 0xac, 0x00};
    const UInt16 selectors[] = {134, 2};
    for (const UInt16 selector : selectors) {
        IOUSBDevRequest clock{0x22, 0x01, 0x0100, selector, sizeof(rate), rate, 0};
        const IOReturn clockResult = (*interface_)->ControlRequest(interface_, 0, &clock);
        if (clockResult != kIOReturnSuccess) return clockResult;
    }

    UInt8 currentMode = 0;
    IOUSBDevRequest read{0xc0, 0x49, 0, 0, sizeof(currentMode), &currentMode, 0};
    result = (*interface_)->ControlRequest(interface_, 0, &read);
    if (result != kIOReturnSuccess) return result;

    const UInt16 selectedMode = static_cast<UInt16>((currentMode & ~0x20u) | 0x30u);
    IOUSBDevRequest write{0x40, 0x49, selectedMode, 0, 0, nullptr, 0};
    result = (*interface_)->ControlRequest(interface_, 0, &write);
    if (result == kIOReturnSuccess && !audioModeChanged_) {
        previousAudioMode_ = currentMode;
        audioModeChanged_ = true;
    }
    return result;
}

IOReturn NS6DeviceSession::open() {
    if (opened_) return kIOReturnExclusiveAccess;

    io_iterator_t iterator = IO_OBJECT_NULL;
    const kern_return_t matching = IOServiceGetMatchingServices(
            kIOMainPortDefault, IOServiceMatching("IOUSBHostInterface"), &iterator);
    if (matching != KERN_SUCCESS) return matching;

    IOReturn result = kIOReturnNotFound;
    std::uint64_t deviceID = 0;
    interface_ = matchingInterface(iterator, 0, nullptr, &deviceID, result);
    IOObjectRelease(iterator);
    if (interface_ == nullptr && result == kIOReturnNotFound) {
        result = configureDevice();
        if (result != kIOReturnSuccess) return result;
        for (unsigned attempt = 0; attempt < 20 && interface_ == nullptr; ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            iterator = IO_OBJECT_NULL;
            const kern_return_t retry = IOServiceGetMatchingServices(
                    kIOMainPortDefault, IOServiceMatching("IOUSBHostInterface"), &iterator);
            if (retry != KERN_SUCCESS) return retry;
            interface_ = matchingInterface(iterator, 0, nullptr, &deviceID, result);
            IOObjectRelease(iterator);
        }
    }
    if (interface_ == nullptr) return result;

    result = (*interface_)->USBInterfaceOpenSeize(interface_);
    if (result != kIOReturnSuccess) {
        (*interface_)->Release(interface_);
        interface_ = nullptr;
        return result;
    }
    opened_ = true;

    result = (*interface_)->SetAlternateInterface(interface_, 1);
    if (result != kIOReturnSuccess) {
        close();
        return result;
    }

    UInt8 endpointCount = 0;
    result = (*interface_)->GetNumEndpoints(interface_, &endpointCount);
    if (result != kIOReturnSuccess) {
        close();
        return result;
    }
    for (UInt8 pipe = 1; pipe <= endpointCount; ++pipe) {
        UInt8 direction = 0;
        UInt8 number = 0;
        UInt8 transferType = 0;
        UInt16 maxPacketSize = 0;
        UInt8 interval = 0;
        result = (*interface_)->GetPipeProperties(interface_, pipe, &direction, &number,
                                                  &transferType, &maxPacketSize, &interval);
        if (result != kIOReturnSuccess) {
            close();
            return result;
        }
        if (direction == kUSBOut && number == 2 && transferType == kUSBIsoc &&
            maxPacketSize == 156 && interval == 1) {
            audioOutPipe_ = pipe;
        } else if (direction == kUSBIn && number == 3 && transferType == kUSBBulk) {
            midiInPipe_ = pipe;
        } else if (direction == kUSBOut && number == 4 && transferType == kUSBBulk &&
                   maxPacketSize == 512) {
            midiOutPipe_ = pipe;
        }
    }
    if (audioOutPipe_ == 0) {
        close();
        return kIOReturnUnsupported;
    }

    result = (*interface_)->CreateInterfaceAsyncEventSource(interface_, &asyncEventSource_);
    if (result != kIOReturnSuccess) {
        close();
        return result;
    }

    iterator = IO_OBJECT_NULL;
    const kern_return_t auxiliaryMatching = IOServiceGetMatchingServices(
            kIOMainPortDefault, IOServiceMatching("IOUSBHostInterface"), &iterator);
    if (auxiliaryMatching != KERN_SUCCESS) {
        close();
        return auxiliaryMatching;
    }
    auxiliaryInterface_ = matchingInterface(iterator, 1, &deviceID, nullptr, result);
    IOObjectRelease(iterator);
    if (auxiliaryInterface_ == nullptr) {
        close();
        return result;
    }

    result = (*auxiliaryInterface_)->USBInterfaceOpenSeize(auxiliaryInterface_);
    if (result != kIOReturnSuccess) {
        (*auxiliaryInterface_)->Release(auxiliaryInterface_);
        auxiliaryInterface_ = nullptr;
        close();
        return result;
    }
    auxiliaryOpened_ = true;
    result = (*auxiliaryInterface_)->SetAlternateInterface(auxiliaryInterface_, 1);
    if (result != kIOReturnSuccess) {
        close();
        return result;
    }

    result = (*auxiliaryInterface_)->GetNumEndpoints(auxiliaryInterface_, &endpointCount);
    if (result != kIOReturnSuccess) {
        close();
        return result;
    }
    for (UInt8 pipe = 1; pipe <= endpointCount; ++pipe) {
        UInt8 direction = 0;
        UInt8 number = 0;
        UInt8 transferType = 0;
        UInt16 maxPacketSize = 0;
        UInt8 interval = 0;
        result = (*auxiliaryInterface_)->GetPipeProperties(auxiliaryInterface_, pipe, &direction,
                                                           &number, &transferType,
                                                           &maxPacketSize, &interval);
        if (result != kIOReturnSuccess) {
            close();
            return result;
        }
        if (direction == kUSBIn && number == 1 && transferType == kUSBIsoc &&
            maxPacketSize == 64 && interval == 4) {
            feedbackPipe_ = pipe;
        } else if (direction == kUSBIn && number == 6 && transferType == kUSBBulk &&
                   maxPacketSize == 512) {
            audioInPipe_ = pipe;
        }
    }
    if (feedbackPipe_ == 0 || audioInPipe_ == 0) {
        close();
        return kIOReturnUnsupported;
    }

    result = (*auxiliaryInterface_)->CreateInterfaceAsyncEventSource(
            auxiliaryInterface_, &auxiliaryAsyncEventSource_);
    if (result != kIOReturnSuccess) {
        close();
        return result;
    }
    return kIOReturnSuccess;
}

IOReturn NS6DeviceSession::acquireAuxiliaryEvents(CFRunLoopRef runLoop) noexcept {
    if (runLoop == nullptr || auxiliaryAsyncEventSource_ == nullptr) return kIOReturnNotReady;
    if (auxiliaryEventClients_ != 0 && auxiliaryEventRunLoop_ != runLoop)
        return kIOReturnNotReady;
    if (auxiliaryEventClients_ == 0) {
        auxiliaryEventRunLoop_ = runLoop;
        CFRetain(auxiliaryEventRunLoop_);
        CFRunLoopAddSource(auxiliaryEventRunLoop_, auxiliaryAsyncEventSource_,
                           kCFRunLoopDefaultMode);
    }
    ++auxiliaryEventClients_;
    return kIOReturnSuccess;
}

IOReturn NS6DeviceSession::acquireEvents(CFRunLoopRef runLoop) noexcept {
    if (runLoop == nullptr || asyncEventSource_ == nullptr) return kIOReturnNotReady;
    if (eventClients_ != 0 && eventRunLoop_ != runLoop) return kIOReturnNotReady;
    if (eventClients_ == 0) {
        eventRunLoop_ = runLoop;
        CFRetain(eventRunLoop_);
        CFRunLoopAddSource(eventRunLoop_, asyncEventSource_, kCFRunLoopDefaultMode);
    }
    ++eventClients_;
    return kIOReturnSuccess;
}

IOReturn NS6DeviceSession::releaseEvents(CFRunLoopRef runLoop) noexcept {
    if (eventClients_ == 0 || eventRunLoop_ != runLoop) return kIOReturnNotReady;
    --eventClients_;
    if (eventClients_ == 0) {
        CFRunLoopRemoveSource(eventRunLoop_, asyncEventSource_, kCFRunLoopDefaultMode);
        CFRelease(eventRunLoop_);
        eventRunLoop_ = nullptr;
    }
    return kIOReturnSuccess;
}

IOReturn NS6DeviceSession::releaseAuxiliaryEvents(CFRunLoopRef runLoop) noexcept {
    if (auxiliaryEventClients_ == 0 || auxiliaryEventRunLoop_ != runLoop)
        return kIOReturnNotReady;
    --auxiliaryEventClients_;
    if (auxiliaryEventClients_ == 0) {
        CFRunLoopRemoveSource(auxiliaryEventRunLoop_, auxiliaryAsyncEventSource_,
                              kCFRunLoopDefaultMode);
        CFRelease(auxiliaryEventRunLoop_);
        auxiliaryEventRunLoop_ = nullptr;
    }
    return kIOReturnSuccess;
}

IOReturn NS6DeviceSession::close() noexcept {
    IOReturn result = kIOReturnSuccess;
    if (audioModeChanged_ && opened_ && interface_ != nullptr) {
        IOUSBDevRequest restore{0x40, 0x49, previousAudioMode_, 0, 0, nullptr, 0};
        result = (*interface_)->ControlRequest(interface_, 0, &restore);
        audioModeChanged_ = false;
    }
    if (eventRunLoop_ != nullptr && asyncEventSource_ != nullptr)
        CFRunLoopRemoveSource(eventRunLoop_, asyncEventSource_, kCFRunLoopDefaultMode);
    if (eventRunLoop_ != nullptr) CFRelease(eventRunLoop_);
    eventRunLoop_ = nullptr;
    eventClients_ = 0;
    if (auxiliaryEventRunLoop_ != nullptr && auxiliaryAsyncEventSource_ != nullptr)
        CFRunLoopRemoveSource(auxiliaryEventRunLoop_, auxiliaryAsyncEventSource_,
                              kCFRunLoopDefaultMode);
    if (auxiliaryEventRunLoop_ != nullptr) CFRelease(auxiliaryEventRunLoop_);
    auxiliaryEventRunLoop_ = nullptr;
    auxiliaryEventClients_ = 0;
    if (auxiliaryOpened_ && auxiliaryInterface_ != nullptr) {
        result = (*auxiliaryInterface_)->USBInterfaceClose(auxiliaryInterface_);
        auxiliaryOpened_ = false;
    }
    if (auxiliaryAsyncEventSource_ != nullptr) {
        CFRelease(auxiliaryAsyncEventSource_);
        auxiliaryAsyncEventSource_ = nullptr;
    }
    if (auxiliaryInterface_ != nullptr) {
        (*auxiliaryInterface_)->Release(auxiliaryInterface_);
        auxiliaryInterface_ = nullptr;
    }
    feedbackPipe_ = 0;
    audioInPipe_ = 0;
    if (opened_ && interface_ != nullptr) {
        const IOReturn closeResult = (*interface_)->USBInterfaceClose(interface_);
        if (result == kIOReturnSuccess) result = closeResult;
        opened_ = false;
    }
    if (asyncEventSource_ != nullptr) {
        CFRelease(asyncEventSource_);
        asyncEventSource_ = nullptr;
    }
    if (interface_ != nullptr) {
        (*interface_)->Release(interface_);
        interface_ = nullptr;
    }
    audioOutPipe_ = 0;
    midiInPipe_ = 0;
    midiOutPipe_ = 0;
    return result;
}
