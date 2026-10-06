#include "NS6USBEndpoint.h"
#include <assert.h>

int main(void) {
    assert(ns6_is_waveform_endpoint(kUSBIn, 6, kUSBBulk));
    assert(!ns6_is_waveform_endpoint(kUSBOut, 6, kUSBBulk));
    assert(!ns6_is_waveform_endpoint(kUSBIn, 6, kUSBIsoc));
    assert(!ns6_is_waveform_endpoint(kUSBIn, 3, kUSBBulk));
    assert(!ns6_should_run_waveform_drain(true));
    assert(ns6_should_run_waveform_drain(false));
    return 0;
}
