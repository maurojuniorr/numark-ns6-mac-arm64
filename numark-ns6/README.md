# Numark NS6

This is a clean implementation tree. No driver source or binary from a prior NS6 version is part of this target.

## Current milestone

Audio OUT and feedback are independent modules; `AudioTransportRuntime` composes their start/stop lifecycle while sharing the output ring with the CoreAudio mix callback. The new `AudioServerPlugIn` publishes a four-channel Float32 output and a separate two-channel Float32 input at 44.1 kHz. StartIO performs the observed USB setup/handshake and starts output, feedback, and the input reader; the last StopIO stops these workers and closes the USB session. The bundle's loading condition matches NS6 USB vendor/product IDs so the HAL can load it when the controller appears. Audio IN uses its own decoder, capture ring/engine, and bulk reader for `0x86`; it does not share the playback ring or callback. The decoder expands the two observed 24-bit bit lanes from each 64-byte stride into stereo Float32. The session refreshes USB configuration 1 when interface services are absent, selects 44.1 kHz clock sources 134 and 2, reads device capabilities, negotiates input mode `0x32`, and restores the prior input mode when it closes. Before capture reads, the reader clears the `0x86` halt. MIDI activation queues four reads on `0x83` before sending host-identify/jog-enable/wake.

The HAL bundle and package include only the audio driver; input is exposed as the stereo master/record return, not separate mic/line channels. This input path has not yet been validated on hardware. The NS6's Master/Record return is a hardware mix of software playback routed through the controller and analog sources sent to Master; it is not driver-side USB OUT copying.

MIDI IN is a separate worker on endpoint `0x83`, with four ordered asynchronous reads and a parser for the observed 42-byte reports. The HAL process forwards complete channel-voice messages over loopback UDP to a per-user-session bridge; that bridge publishes “Numark NS6 In” through CoreMIDI. CoreMIDI publication must stay outside the HAL process, which cannot create desktop-visible endpoints. SysEx and system-common messages are not yet forwarded. The worker starts with audio `StartIO` and stops before the shared USB session closes. Hardware delivery still needs testing with the packaged bridge and a MIDI monitor.

The ring is bounded and allocated at construction. Its audio-side `read` and producer-side `write` do not allocate, block, or perform USB I/O. It requires exactly one producer and one consumer. Reads may be short; the USB packetizer fills missing frames with silence.

Build and run its tests:

```sh
cmake -S numark-ns6 -B /tmp/ns6-audio-out-build -DNS6_ENABLE_SANITIZERS=ON
cmake --build /tmp/ns6-audio-out-build
ctest --test-dir /tmp/ns6-audio-out-build --output-on-failure
```

Build and package the experimental modular audio driver with `tools/build-package.sh`.

After uninstalling other NS6 drivers and freeing the USB interface, run `/tmp/ns6-audio-out-build/ns6_feedback_probe 20` with administrator authorization to record endpoint `0x81` without sending audio. Add `--silent-output` to transmit silence through the new OUT worker while reading feedback; this is useful for checking whether feedback becomes active with playback but is still a probe, not a driver package.
