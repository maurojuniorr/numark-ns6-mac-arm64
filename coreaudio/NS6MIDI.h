#ifndef NS6_MIDI_H
#define NS6_MIDI_H

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/usb/IOUSBLib.h>
#include <stdbool.h>

/* The USB transport owns the interface. CoreMIDI is started only after that
 * interface has been configured and its async event source is on this run loop. */
bool ns6_midi_start(IOUSBInterfaceInterface **usb, CFRunLoopRef run_loop);
void ns6_midi_stop(void);

#endif
