# Numark NS6

This is a clean implementation tree. No driver source or binary from a prior NS6 version is part of this target.

## Current milestone

Audio OUT and feedback are independent modules; `AudioTransportRuntime` composes their start/stop lifecycle while sharing the output ring with the CoreAudio mix callback. The new `AudioServerPlugIn` publishes a four-channel Float32 output and a separate two-channel Float32 input at 44.1 kHz. StartIO performs the observed USB setup/handshake and starts output and feedback; bulk input reads run only while the separate input device has active clients. The last StopIO stops these workers and closes the USB session. The bundle's loading condition matches NS6 USB vendor/product IDs so the HAL can load it when the controller appears. Audio IN uses its own decoder, capture ring/engine, and bulk reader for `0x86`; it does not share the playback ring or callback. The decoder expands the two observed 24-bit bit lanes from each 64-byte stride into stereo Float32. The session refreshes USB configuration 1 when interface services are absent, selects 44.1 kHz clock sources 134 and 2, reads device capabilities, negotiates input mode `0x32`, and restores the prior input mode when it closes. Before capture reads, the reader clears the `0x86` halt. MIDI activation queues four reads on `0x83` before sending host-identify/jog-enable/wake.

The input is exposed as the stereo master/record return, not separate mic/line channels. Hardware testing confirmed capture in OBS and TikTok LIVE Studio while Mixxx continued output and MIDI control. The NS6's Master/Record return is a hardware mix of software playback routed through the controller and analog sources sent to Master; it is not driver-side USB OUT copying.

The current 0.2.45 milestone and the issues resolved during validation are recorded in [the 1.0.0 release-candidate notes](docs/releases/1.0.0-rc.1.md).

MIDI IN uses endpoint `0x83`, four ordered asynchronous reads, and the observed 42-byte report parser. The HAL forwards complete channel-voice messages over loopback UDP to the per-user-session CoreMIDI bridge. Read errors trigger a pipe drain and restart on the runtime thread, outside the USB callback. MIDI OUT uses a separate worker on endpoint `0x04`; the bridge batches host messages into at most 41 payload bytes, and the worker wraps each datagram in the NS6's 42-byte report before writing it to USB. Both virtual CoreMIDI endpoints use the original “Numark NS6” identity so existing Mixxx mappings can bind. The audio OUT and IN devices remain separate. Input SysEx/system-common messages are not forwarded yet, and USB disconnection can still lose in-flight MIDI; the driver logs read, forwarding, and write failures.

The package includes this bridge and a LaunchAgent that starts it at login. While running, it checks the NS6 USB and CoreAudio output UID, then holds `AudioDeviceStart(..., NULL)` as a silent client. It releases the client on disconnect and starts it again when the device is enumerated after reconnection. This uses a small periodic check (250 ms while inactive, 1 s while active) and keeps CoreAudio rendering while the NS6 is connected. Package updates stop the legacy device-service daemon and remove the old `NumarkNS6M1.driver` before restarting CoreAudio, so it cannot retain exclusive access to the USB interface. The package also installs or replaces `/Applications/Numark NS6 Status.app`, which shows the firmware version and identified active audio clients from the two CoreAudio devices.

To remove this driver and its MIDI bridge, Status app, legacy helpers, and installer receipts before testing another package, run `sudo ./tools/uninstall-driver.sh` from this repository. It asks for confirmation and restarts CoreAudio; logs and user preferences are preserved.

The ring is bounded and allocated at construction. Its audio-side `read` and producer-side `write` do not allocate, block, or perform USB I/O. It requires exactly one producer and one consumer. Reads may be short; the USB packetizer fills missing frames with silence.

Build and run its tests:

```sh
cmake -S numark-ns6 -B /tmp/ns6-audio-out-build -DNS6_ENABLE_SANITIZERS=ON
cmake --build /tmp/ns6-audio-out-build
ctest --test-dir /tmp/ns6-audio-out-build --output-on-failure
```

Build and package the experimental modular audio driver with `tools/build-package.sh`.

After uninstalling other NS6 drivers and freeing the USB interface, run `/tmp/ns6-audio-out-build/ns6_feedback_probe 20` with administrator authorization to record endpoint `0x81` without sending audio. Add `--silent-output` to transmit silence through the new OUT worker while reading feedback; this is useful for checking whether feedback becomes active with playback but is still a probe, not a driver package.
