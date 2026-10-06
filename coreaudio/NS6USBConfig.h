#ifndef NS6_USB_CONFIG_H
#define NS6_USB_CONFIG_H

#include <stdbool.h>

#ifndef NS6_MIDI_ENABLED
#define NS6_MIDI_ENABLED 1
#endif

#ifndef NS6_FEEDBACK_READER_ENABLED
#define NS6_FEEDBACK_READER_ENABLED 1
#endif

#ifndef NS6_USB_OUTPUT_SLOT_COUNT
#define NS6_USB_OUTPUT_SLOT_COUNT 8
#endif

static inline bool ns6_usb_midi_input_enabled(void) {
    return NS6_MIDI_ENABLED != 0;
}

static inline bool ns6_usb_feedback_reader_enabled(void) {
    return NS6_FEEDBACK_READER_ENABLED != 0;
}

static inline unsigned ns6_usb_output_slot_count(void) {
    return NS6_USB_OUTPUT_SLOT_COUNT;
}

#endif
