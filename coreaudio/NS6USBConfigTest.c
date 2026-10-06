#include "NS6USBConfig.h"
#include <assert.h>

int main(void) {
    assert(ns6_usb_midi_input_enabled() == (NS6_MIDI_ENABLED != 0));
    assert(ns6_usb_feedback_reader_enabled() == (NS6_FEEDBACK_READER_ENABLED != 0));
    assert(ns6_usb_output_slot_count() == NS6_USB_OUTPUT_SLOT_COUNT);
    return 0;
}
