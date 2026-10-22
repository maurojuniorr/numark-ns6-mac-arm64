#include <CoreMIDI/CoreMIDI.h>
#include <CoreAudio/AudioHardware.h>
#include <IOKit/IOKitLib.h>
#include <os/log.h>

#include <arpa/inet.h>
#include <poll.h>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cerrno>
#include <cstring>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

#include "AudioKeepalivePolicy.hpp"

namespace {
constexpr std::uint16_t kBridgePort = 48239;
constexpr AudioDeviceID kUnknownDevice = kAudioObjectUnknown;
constexpr SInt32 kNS6Vendor = 5604;
constexpr SInt32 kNS6Product = 121;
const CFStringRef kNS6OutputUID = CFSTR("io.github.maurojuniorr.numark-ns6.out");

bool ns6USBPresent() {
    CFMutableDictionaryRef matching = IOServiceMatching("IOUSBHostDevice");
    if (matching == nullptr) return false;
    CFNumberRef vendor = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &kNS6Vendor);
    CFNumberRef product = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &kNS6Product);
    if (vendor == nullptr || product == nullptr) {
        if (vendor != nullptr) CFRelease(vendor);
        if (product != nullptr) CFRelease(product);
        CFRelease(matching);
        return false;
    }
    CFDictionarySetValue(matching, CFSTR("idVendor"), vendor);
    CFDictionarySetValue(matching, CFSTR("idProduct"), product);
    CFRelease(vendor);
    CFRelease(product);
    const io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault, matching);
    if (service == IO_OBJECT_NULL) return false;
    IOObjectRelease(service);
    return true;
}

AudioDeviceID findNS6Output() {
    if (!ns6USBPresent()) return kUnknownDevice;
    const AudioObjectPropertyAddress address{kAudioHardwarePropertyDevices,
        kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &address, 0, nullptr, &size) != noErr ||
        size < sizeof(AudioDeviceID)) return kUnknownDevice;

    std::vector<AudioDeviceID> devices(size / sizeof(AudioDeviceID));
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, nullptr, &size,
                                   devices.data()) != noErr) return kUnknownDevice;

    const AudioObjectPropertyAddress uidAddress{kAudioDevicePropertyDeviceUID,
        kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    for (AudioDeviceID device : devices) {
        CFStringRef uid = nullptr;
        UInt32 uidSize = sizeof(uid);
        if (AudioObjectGetPropertyData(device, &uidAddress, 0, nullptr, &uidSize, &uid) == noErr &&
            uid != nullptr) {
            const bool matches = CFEqual(uid, kNS6OutputUID);
            CFRelease(uid);
            if (matches) return device;
        }
    }
    return kUnknownDevice;
}

AudioDeviceID updateAudioKeepalive(AudioDeviceID activeDevice) {
    static OSStatus lastStartFailure = noErr;
    const AudioDeviceID discoveredDevice = findNS6Output();
    const auto action = ns6::audioKeepaliveAction(activeDevice, discoveredDevice);
    if (action == ns6::AudioKeepaliveAction::Stop) {
        const OSStatus status = AudioDeviceStop(activeDevice, nullptr);
        os_log(OS_LOG_DEFAULT, "Numark NS6 MIDI Bridge stopped audio keepalive (device %u, status %d)",
               activeDevice, (int)status);
        activeDevice = kUnknownDevice;
    }
    if (action == ns6::AudioKeepaliveAction::Start) {
        const OSStatus status = AudioDeviceStart(discoveredDevice, nullptr);
        if (status == noErr) {
            lastStartFailure = noErr;
            os_log(OS_LOG_DEFAULT, "Numark NS6 MIDI Bridge started audio keepalive (device %u)",
                   discoveredDevice);
            activeDevice = discoveredDevice;
        } else if (status != lastStartFailure) {
            lastStartFailure = status;
            os_log_error(OS_LOG_DEFAULT, "Numark NS6 MIDI Bridge could not start audio keepalive (device %u, status %d)",
                         discoveredDevice, (int)status);
        }
    }
    return activeDevice;
}
}

int main() {
    const int socketFD = socket(AF_INET, SOCK_DGRAM, 0);
    if (socketFD < 0) {
        std::fprintf(stderr, "MIDI bridge socket: %s\n", std::strerror(errno));
        return 1;
    }
    sockaddr_in address{};
    address.sin_len = sizeof(address);
    address.sin_family = AF_INET;
    address.sin_port = htons(kBridgePort);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(socketFD, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        std::fprintf(stderr, "MIDI bridge bind: %s\n", std::strerror(errno));
        close(socketFD);
        return 1;
    }

    MIDIClientRef client = 0;
    MIDIEndpointRef source = 0;
    OSStatus status = MIDIClientCreate(CFSTR("Numark NS6 MIDI Bridge"), nullptr, nullptr, &client);
    if (status == noErr) status = MIDISourceCreate(client, CFSTR("Numark NS6 In"), &source);
    if (status != noErr) {
        if (source) MIDIEndpointDispose(source);
        if (client) MIDIClientDispose(client);
        close(socketFD);
        std::fprintf(stderr, "Numark NS6 MIDI bridge CoreMIDI error: %d\n", (int)status);
        return 1;
    }
    MIDIObjectSetStringProperty(source, kMIDIPropertyManufacturer, CFSTR("Numark"));
    MIDIObjectSetStringProperty(source, kMIDIPropertyModel, CFSTR("NS6"));
    MIDIObjectSetStringProperty(source, kMIDIPropertyDisplayName, CFSTR("Numark NS6 In"));
    MIDIObjectSetIntegerProperty(source, kMIDIPropertyUniqueID, 0x4e533601);

    alignas(MIDIPacketList) std::uint8_t storage[512]{};
    std::uint8_t input[128]{};
    AudioDeviceID activeDevice = kUnknownDevice;
    auto nextKeepaliveCheck = std::chrono::steady_clock::now();
    for (;;) {
        pollfd descriptor{socketFD, POLLIN, 0};
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            nextKeepaliveCheck - std::chrono::steady_clock::now()).count();
        const int timeout = static_cast<int>(remaining > 0 ? remaining : 0);
        const int ready = poll(&descriptor, 1, timeout);
        if (ready < 0 && errno != EINTR) {
            std::fprintf(stderr, "MIDI bridge poll: %s\n", std::strerror(errno));
            break;
        }
        if (ready > 0 && (descriptor.revents & POLLIN) != 0) {
            const ssize_t count = recv(socketFD, input, sizeof(input), 0);
            if (count > 0) {
                auto* list = reinterpret_cast<MIDIPacketList*>(storage);
                MIDIPacket* packet = MIDIPacketListInit(list);
                if (MIDIPacketListAdd(list, sizeof(storage), packet, 0, (UInt16)count, input) != nullptr)
                    (void)MIDIReceived(source, list);
            }
        }
        if (std::chrono::steady_clock::now() >= nextKeepaliveCheck) {
            activeDevice = updateAudioKeepalive(activeDevice);
            nextKeepaliveCheck = std::chrono::steady_clock::now() +
                (activeDevice == kUnknownDevice ? std::chrono::milliseconds(250)
                                                : std::chrono::milliseconds(1000));
        }
    }
    if (activeDevice != kUnknownDevice) (void)AudioDeviceStop(activeDevice, nullptr);
    MIDIEndpointDispose(source);
    MIDIClientDispose(client);
    close(socketFD);
    return 1;
}
