#ifndef NS6_USB_ENDPOINT_H
#define NS6_USB_ENDPOINT_H

#include <IOKit/usb/USB.h>
#include <stdbool.h>

static inline bool ns6_is_waveform_endpoint(UInt8 direction, UInt8 number, UInt8 type) {
    return direction == kUSBIn && number == 6 && type == kUSBBulk;
}

static inline bool ns6_should_run_waveform_drain(bool midi_session_active) {
    return !midi_session_active;
}

#endif
