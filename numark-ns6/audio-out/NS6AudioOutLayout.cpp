#include "NS6AudioOutLayout.hpp"

std::uint32_t ns6AudioOutServiceRateHz(const NS6AudioOutEndpointDescriptor& endpoint) noexcept {
    if (endpoint.vendorId != 0x15e4 || endpoint.productId != 0x0079 ||
        endpoint.speed != USBSpeed::High || endpoint.interfaceNumber != 0 ||
        endpoint.alternateSetting != 1 || endpoint.endpointAddress != 0x02 ||
        endpoint.transferType != USBTransferType::Isochronous || endpoint.maxPacketSize != 156 ||
        endpoint.interval != 1) {
        return 0;
    }
    return 8000;
}
