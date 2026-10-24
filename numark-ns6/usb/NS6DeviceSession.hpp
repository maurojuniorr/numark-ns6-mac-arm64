#pragma once

#include <IOKit/IOKitLib.h>
#include <IOKit/usb/IOUSBLib.h>
#include <cstdint>

class NS6DeviceSession final {
public:
    NS6DeviceSession() = default;
    ~NS6DeviceSession();
    NS6DeviceSession(const NS6DeviceSession&) = delete;
    NS6DeviceSession& operator=(const NS6DeviceSession&) = delete;

    IOReturn open();
    bool isCurrentDevice() const noexcept;
    static bool devicePresent() noexcept;
    IOReturn configureAudioInputMode() noexcept;
    // Call only after endpoint workers cancel and drain their in-flight requests.
    IOReturn close() noexcept;
    IOReturn acquireEvents(CFRunLoopRef runLoop) noexcept;
    IOReturn releaseEvents(CFRunLoopRef runLoop) noexcept;
    IOReturn acquireAuxiliaryEvents(CFRunLoopRef runLoop) noexcept;
    IOReturn releaseAuxiliaryEvents(CFRunLoopRef runLoop) noexcept;
    IOUSBInterfaceInterface** interface() const noexcept { return interface_; }
    UInt8 audioOutPipe() const noexcept { return audioOutPipe_; }
    UInt8 midiInPipe() const noexcept { return midiInPipe_; }
    UInt8 midiOutPipe() const noexcept { return midiOutPipe_; }
    CFRunLoopSourceRef asyncEventSource() const noexcept { return asyncEventSource_; }
    IOUSBInterfaceInterface** auxiliaryInterface() const noexcept { return auxiliaryInterface_; }
    UInt8 feedbackPipe() const noexcept { return feedbackPipe_; }
    UInt8 audioInPipe() const noexcept { return audioInPipe_; }
    const char* firmwareVersion() const noexcept { return firmwareVersion_; }
    CFRunLoopSourceRef auxiliaryAsyncEventSource() const noexcept {
        return auxiliaryAsyncEventSource_;
    }

private:
    IOUSBInterfaceInterface** interface_ = nullptr;
    CFRunLoopSourceRef asyncEventSource_ = nullptr;
    UInt8 audioOutPipe_ = 0;
    UInt8 midiInPipe_ = 0;
    UInt8 midiOutPipe_ = 0;
    bool opened_ = false;
    CFRunLoopRef eventRunLoop_ = nullptr;
    UInt32 eventClients_ = 0;
    IOUSBInterfaceInterface** auxiliaryInterface_ = nullptr;
    CFRunLoopSourceRef auxiliaryAsyncEventSource_ = nullptr;
    UInt8 feedbackPipe_ = 0;
    UInt8 audioInPipe_ = 0;
    bool auxiliaryOpened_ = false;
    char firmwareVersion_[32]{};
    UInt16 previousAudioMode_ = 0;
    bool audioModeChanged_ = false;
    std::uint64_t deviceID_ = 0;
    CFRunLoopRef auxiliaryEventRunLoop_ = nullptr;
    UInt32 auxiliaryEventClients_ = 0;
};
