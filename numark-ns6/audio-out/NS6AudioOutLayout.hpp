#pragma once

#include <cstdint>

enum class USBSpeed : std::uint8_t { Unknown, Full, High, Super };
enum class USBTransferType : std::uint8_t { Control, Isochronous, Bulk, Interrupt };

struct NS6AudioOutEndpointDescriptor final {
    std::uint16_t vendorId;
    std::uint16_t productId;
    USBSpeed speed;
    std::uint8_t interfaceNumber;
    std::uint8_t alternateSetting;
    std::uint8_t endpointAddress;
    USBTransferType transferType;
    std::uint16_t maxPacketSize;
    std::uint8_t interval;
};

// Returns the endpoint's service intervals per second, or zero for an unsupported layout.
std::uint32_t ns6AudioOutServiceRateHz(const NS6AudioOutEndpointDescriptor& endpoint) noexcept;
