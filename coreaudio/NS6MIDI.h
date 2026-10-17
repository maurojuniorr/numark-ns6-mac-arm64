#ifndef NS6_MIDI_H
#define NS6_MIDI_H

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/usb/IOUSBLib.h>
#include <stdbool.h>

/* The USB transport owns the interface.  MIDI is forwarded over loopback to
 * the user-session bridge, because HAL plug-ins run inside coreaudiod and
 * cannot publish CoreMIDI endpoints visible to desktop applications. */
bool ns6_midi_start(IOUSBInterfaceInterface **usb, CFRunLoopRef run_loop);
bool ns6_midi_handshake(CFRunLoopRef run_loop);
bool ns6_midi_take_handshake_request(void);
void ns6_midi_set_audio_active(bool active);
void ns6_midi_stop(void);

#endif
