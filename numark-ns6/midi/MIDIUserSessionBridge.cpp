#include <CoreMIDI/CoreMIDI.h>

#include <arpa/inet.h>
#include <cstdint>
#include <cstdio>
#include <cerrno>
#include <cstring>
#include <sys/socket.h>
#include <unistd.h>

namespace {
constexpr std::uint16_t kBridgePort = 48239;
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
    for (;;) {
        const ssize_t count = recv(socketFD, input, sizeof(input), 0);
        if (count <= 0) continue;
        auto* list = reinterpret_cast<MIDIPacketList*>(storage);
        MIDIPacket* packet = MIDIPacketListInit(list);
        if (MIDIPacketListAdd(list, sizeof(storage), packet, 0, (UInt16)count, input) != nullptr)
            (void)MIDIReceived(source, list);
    }
}
