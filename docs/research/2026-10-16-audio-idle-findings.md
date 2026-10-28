# NS6 audio idle behavior — 2026-10-16

## What the driver now does

In the `codex/audio-on-demand-from-0.2.123` test branch, the first CoreAudio `StartIO` starts the USB audio transport. When the final client calls `StopIO`, the driver clears the active audio flow, pauses PCM consumption, discards queued client audio, and keeps the isochronous USB OUT transport running with silence. A later client can resume that transport without tearing down and reopening the USB interface.

This is why opening or closing one audio client while another remains active no longer needs to stop the shared USB transport. The transport lifetime is separate from the lifetime of an individual CoreAudio client.

## What the latest log showed

During the captured final-client shutdown:

- `StopIO` logged that the transport was retained with silence.
- One audio underrun was recorded at the transition: 5 frames.
- Subsequent health reports showed USB transfers continuing, with no USB transfer errors, isochronous packet errors, or short packets.
- About 334 ms after `StopIO`, MIDI input completions began returning `0xe00002ed` (`kIOReturnNotResponding`); the MIDI input pipe recovery path then cleared stalls repeatedly.

The log therefore does not show the audio transport being stopped when the LED flickers. It does show MIDI input recovery starting shortly after the last audio client closes. That timing makes the MIDI input path a stronger lead for the LED behavior than an audio USB shutdown, but the software log does not directly observe the physical LED, so it is not proof of causation.

## Current lifecycle coupling

The USB worker initializes the audio OUT transport and starts the physical MIDI session together. `ns6_midi_start()` submits the reads for the controller's MIDI IN endpoint and starts the MIDI output writer. Because the audio transport is now retained after the final `StopIO`, this MIDI session also remains active while no audio client is open. The separate CoreMIDI bridge keeps its virtual source and destination published for applications.

This identifies the next investigation: separate physical MIDI input polling from the retained audio transport, and only suspend MIDI input when there is reliable evidence that no application is using the virtual MIDI input. Lack of recent MIDI traffic is not reliable evidence: a DJ application can be connected and idle while waiting for the next button or jog event. Audio OUT should continue independently in silent idle.

## Limits

The captured window is short and shows correlation, not proof that MIDI recovery causes the LED flicker. The 5-frame underrun is a separate, small transition event. Longer hardware testing is still needed after changing MIDI polling lifecycle.
