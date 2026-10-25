#include <CoreMIDI/CoreMIDI.h>
#include <CoreAudio/AudioHardware.h>
#include <IOKit/IOKitLib.h>
#include <os/log.h>

#include <arpa/inet.h>
#include <poll.h>
#include <chrono>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cerrno>
#include <cstring>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>
#include <thread>

#include "AudioKeepalivePolicy.hpp"

namespace {
constexpr std::uint16_t kBridgePort = 48239;
constexpr std::uint16_t kDriverPort = 48238;
int outboundSocket = -1;
sockaddr_in driverAddress{};
std::uint64_t outboundFailures = 0;

void sendToDriver(const std::uint8_t* bytes, std::size_t length) {
    if (length == 0) return;
    const ssize_t sent = sendto(outboundSocket, bytes, length, MSG_DONTWAIT,
                                reinterpret_cast<const sockaddr*>(&driverAddress),
                                sizeof(driverAddress));
    if (sent != static_cast<ssize_t>(length) && (++outboundFailures == 1 || outboundFailures % 100 == 0))
        os_log_error(OS_LOG_DEFAULT, "NS6 MIDI bridge outbound datagram failed: %zd/%zu (failures %llu)",
                     sent, length, outboundFailures);
}

unsigned midiMessageLength(std::uint8_t status) {
    if (status >= 0x80 && status <= 0xef)
        return (status & 0xf0) == 0xc0 || (status & 0xf0) == 0xd0 ? 2 : 3;
    return 0;
}

void receiveFromApplication(const MIDIPacketList* list, void*, void*) {
    std::uint8_t report[41]{};
    std::size_t used = 0;
    const MIDIPacket* packet = list->packet;
    for (UInt32 index = 0; index < list->numPackets; ++index, packet = MIDIPacketNext(packet)) {
        for (UInt16 offset = 0; offset < packet->length;) {
            const std::uint8_t status = packet->data[offset];
            if (status == 0xf0) {
                sendToDriver(report, used);
                used = 0;
                const std::size_t remaining = packet->length - offset;
                for (std::size_t part = 0; part < remaining; part += sizeof(report)) {
                    const std::size_t count = std::min(sizeof(report), remaining - part);
                    sendToDriver(packet->data + offset + part, count);
                }
                break;
            }
            const unsigned length = midiMessageLength(status);
            if (length == 0 || offset + length > packet->length) { ++offset; continue; }
            if (used + length > sizeof(report)) { sendToDriver(report, used); used = 0; }
            std::memcpy(report + used, packet->data + offset, length);
            used += length;
            offset += length;
        }
    }
    sendToDriver(report, used);
}
constexpr AudioDeviceID kUnknownDevice = kAudioObjectUnknown;
constexpr SInt32 kNS6Vendor = 5604;
constexpr SInt32 kNS6Product = 121;
const CFStringRef kNS6OutputUID = CFSTR("io.github.maurojuniorr.numark-ns6.out");
const CFStringRef kNS6InputUID = CFSTR("io.github.maurojuniorr.numark-ns6.in");

std::uint64_t ns6USBGeneration() {
    CFMutableDictionaryRef matching = IOServiceMatching("IOUSBHostDevice");
    if (matching == nullptr) return 0;
    CFNumberRef vendor = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &kNS6Vendor);
    CFNumberRef product = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &kNS6Product);
    if (vendor == nullptr || product == nullptr) {
        if (vendor != nullptr) CFRelease(vendor);
        if (product != nullptr) CFRelease(product);
        CFRelease(matching);
        return 0;
    }
    CFDictionarySetValue(matching, CFSTR("idVendor"), vendor);
    CFDictionarySetValue(matching, CFSTR("idProduct"), product);
    CFRelease(vendor);
    CFRelease(product);
    const io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault, matching);
    if (service == IO_OBJECT_NULL) return 0;
    std::uint64_t generation = 0;
    (void)IORegistryEntryGetRegistryEntryID(service, &generation);
    IOObjectRelease(service);
    return generation;
}

AudioDeviceID findNS6Device(CFStringRef targetUID) {
    if (ns6USBGeneration() == 0) return kUnknownDevice;
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
            const bool matches = CFEqual(uid, targetUID);
            CFRelease(uid);
            if (matches) return device;
        }
    }
    return kUnknownDevice;
}

struct KeepaliveState {
    AudioDeviceID activeDevice = kUnknownDevice;
    OSStatus lastStartFailure = noErr;
    std::uint64_t activeUSBGeneration = 0;
    unsigned missingFlowChecks = 0;
};

bool hasActiveAudioFlow(AudioDeviceID device) {
    const AudioObjectPropertyAddress address{0x6e733666, // 'ns6f'
        kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    CFStringRef frames = nullptr;
    UInt32 size = sizeof(frames);
    const OSStatus status = AudioObjectGetPropertyData(device, &address, 0, nullptr, &size, &frames);
    const bool active = status == noErr && frames != nullptr && CFStringGetIntValue(frames) > 0;
    if (frames != nullptr) CFRelease(frames);
    return active;
}

void updateAudioKeepalive(KeepaliveState& state, CFStringRef uid, const char* direction) {
    const std::uint64_t usbGeneration = ns6USBGeneration();
    const AudioDeviceID discoveredDevice = findNS6Device(uid);
    if (state.activeDevice != kUnknownDevice && state.activeDevice == discoveredDevice &&
        usbGeneration != 0 && usbGeneration == state.activeUSBGeneration) {
        if (hasActiveAudioFlow(state.activeDevice)) {
            state.missingFlowChecks = 0;
        } else if (++state.missingFlowChecks >= 2) {
            os_log_error(OS_LOG_DEFAULT,
                         "Numark NS6 MIDI Bridge %{public}s keepalive has no active audio flow; restarting",
                         direction);
            (void)AudioDeviceStop(state.activeDevice, nullptr);
            state.activeDevice = kUnknownDevice;
            state.activeUSBGeneration = 0;
            state.missingFlowChecks = 0;
        }
    }
    const auto action = ns6::audioKeepaliveAction(
        state.activeDevice, discoveredDevice, usbGeneration == state.activeUSBGeneration);
    if (action == ns6::AudioKeepaliveAction::Stop) {
        const OSStatus status = AudioDeviceStop(state.activeDevice, nullptr);
        os_log(OS_LOG_DEFAULT, "Numark NS6 MIDI Bridge stopped %{public}s keepalive (device %u, status %d)",
               direction, state.activeDevice, (int)status);
        state.activeDevice = kUnknownDevice;
        state.activeUSBGeneration = 0;
        state.missingFlowChecks = 0;
    }
    if (action == ns6::AudioKeepaliveAction::Start) {
        const OSStatus status = AudioDeviceStart(discoveredDevice, nullptr);
        if (status == noErr) {
            state.lastStartFailure = noErr;
            os_log(OS_LOG_DEFAULT, "Numark NS6 MIDI Bridge started %{public}s keepalive (device %u)",
                   direction, discoveredDevice);
            state.activeDevice = discoveredDevice;
            state.activeUSBGeneration = usbGeneration;
            state.missingFlowChecks = 0;
        } else if (status != state.lastStartFailure) {
            state.lastStartFailure = status;
            os_log_error(OS_LOG_DEFAULT, "Numark NS6 MIDI Bridge could not start %{public}s keepalive (device %u, status %d)",
                         direction, discoveredDevice, (int)status);
        }
    }
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
    const int bufferBytes = 1024 * 1024;
    (void)setsockopt(socketFD, SOL_SOCKET, SO_RCVBUF, &bufferBytes, sizeof(bufferBytes));
    outboundSocket = socket(AF_INET, SOCK_DGRAM, 0);
    if (outboundSocket < 0) { close(socketFD); return 1; }
    (void)setsockopt(outboundSocket, SOL_SOCKET, SO_SNDBUF, &bufferBytes, sizeof(bufferBytes));
    driverAddress.sin_len = sizeof(driverAddress);
    driverAddress.sin_family = AF_INET;
    driverAddress.sin_port = htons(kDriverPort);
    driverAddress.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    MIDIClientRef client = 0;
    MIDIEndpointRef source = 0;
    MIDIEndpointRef destination = 0;
    OSStatus status = MIDIClientCreate(CFSTR("Numark NS6 MIDI Bridge"), nullptr, nullptr, &client);
    if (status == noErr) status = MIDISourceCreate(client, CFSTR("Numark NS6"), &source);
    if (status == noErr) status = MIDIDestinationCreate(client, CFSTR("Numark NS6"),
                                                       receiveFromApplication, nullptr, &destination);
    if (status != noErr) {
        if (destination) MIDIEndpointDispose(destination);
        if (source) MIDIEndpointDispose(source);
        if (client) MIDIClientDispose(client);
        close(socketFD);
        close(outboundSocket);
        std::fprintf(stderr, "Numark NS6 MIDI bridge CoreMIDI error: %d\n", (int)status);
        return 1;
    }
    MIDIObjectSetStringProperty(source, kMIDIPropertyManufacturer, CFSTR("Numark"));
    MIDIObjectSetStringProperty(source, kMIDIPropertyModel, CFSTR("NS6"));
    MIDIObjectSetStringProperty(source, kMIDIPropertyDisplayName, CFSTR("Numark NS6 MIDI"));
    MIDIObjectSetIntegerProperty(source, kMIDIPropertyUniqueID, 0x4e533601);
    MIDIObjectSetStringProperty(destination, kMIDIPropertyManufacturer, CFSTR("Numark"));
    MIDIObjectSetStringProperty(destination, kMIDIPropertyModel, CFSTR("NS6"));
    MIDIObjectSetStringProperty(destination, kMIDIPropertyDisplayName, CFSTR("Numark NS6 MIDI"));
    MIDIObjectSetIntegerProperty(destination, kMIDIPropertyUniqueID, 0x4e533602);

    alignas(MIDIPacketList) std::uint8_t storage[512]{};
    std::uint8_t input[128]{};
    std::atomic<bool> keepaliveRunning{true};
    // CoreAudio discovery/start may block; keep it off the MIDI delivery loop.
    std::thread keepalive([&] {
        KeepaliveState output;
        KeepaliveState input;
        while (keepaliveRunning.load(std::memory_order_acquire)) {
            updateAudioKeepalive(output, kNS6OutputUID, "output");
            // AUHAL capture clients can receive callbacks without starting the input device's IO engine.
            updateAudioKeepalive(input, kNS6InputUID, "input");
            std::this_thread::sleep_for(output.activeDevice == kUnknownDevice ||
                                        input.activeDevice == kUnknownDevice
                ? std::chrono::milliseconds(250) : std::chrono::milliseconds(1000));
        }
        if (input.activeDevice != kUnknownDevice) (void)AudioDeviceStop(input.activeDevice, nullptr);
        if (output.activeDevice != kUnknownDevice) (void)AudioDeviceStop(output.activeDevice, nullptr);
    });
    for (;;) {
        pollfd descriptor{socketFD, POLLIN, 0};
        const int ready = poll(&descriptor, 1, -1);
        if (ready < 0 && errno != EINTR) {
            std::fprintf(stderr, "MIDI bridge poll: %s\n", std::strerror(errno));
            break;
        }
        if (ready > 0 && (descriptor.revents & POLLIN) != 0) {
            const ssize_t count = recv(socketFD, input, sizeof(input), 0);
            if (count > 0) {
                auto* list = reinterpret_cast<MIDIPacketList*>(storage);
                MIDIPacket* packet = MIDIPacketListInit(list);
                if (MIDIPacketListAdd(list, sizeof(storage), packet, 0,
                                      (UInt16)count, input) != nullptr) {
                    const OSStatus delivered = MIDIReceived(source, list);
                    if (delivered != noErr) {
                        os_log_error(OS_LOG_DEFAULT, "NS6 MIDI bridge MIDIReceived failed: %d", (int)delivered);
                    }
                }
            }
        }
    }
    keepaliveRunning.store(false, std::memory_order_release);
    keepalive.join();
    MIDIEndpointDispose(destination);
    MIDIEndpointDispose(source);
    MIDIClientDispose(client);
    close(socketFD);
    close(outboundSocket);
    return 1;
}
