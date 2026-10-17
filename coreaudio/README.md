# CoreAudio HAL device

Created and maintained by **Mauro Junior** ([maurojuniorr](https://github.com/maurojuniorr)).

This directory is the macOS output-device layer for the original Numark NS6.

The USB transport has already been validated on Apple Silicon through
`macos/tools/ns6-native-tone.c`: it claims interface 0, selects alternate
setting 1, uses `LowLatencyWriteIsochPipeAsync`, and produces a clean 44.1 kHz
test tone through the NS6.

The HAL plug-in publishes one 4-channel, 44.1 kHz Float32 output device. It
accepts CoreAudio clients' buffer-frame-size requests, including the
period selected in Mixxx, and returns the active frame count through the same
property instead of forcing a constant 96-frame report. NS6 Status shows the
active stream period and its calculated latency; in hardware testing it matched
Mixxx's reported latency. The Status app reads the installed HAL bundle for
the driver version and shows the active CoreAudio cycle for the current app.
It uses recent audio callbacks to detect a flow because the macOS
`DeviceIsRunning` property can remain zero during playback. At 44.1 kHz, 49,
128, 192, 256, 512, and 1024 frames correspond to approximately 1.1, 2.9, 4.4,
5.8, 11.6, and 23.2 ms per buffer. The worker converts the CoreAudio mix to the
packed 24-bit format used by the NS6.
Channels 1/2 feed Master and channels 3/4 feed Headphones independently, so
DJ software can route its main mix and cue mix separately.

At stream start or resume, the worker waits until its internal queue contains
at least 1024 PCM frames (or the configured startup prebuffer, if larger)
before submitting live audio. This is an internal USB-queue priming threshold;
it does not override the buffer period requested by Mixxx or another
CoreAudio client.

The USB worker emits whole audio frames in groups of eight microframes. The
default build reads NS6 feedback endpoint `0x81` and tracks its rolling rate,
keeping the transmitted rate within 44,000.0–44,100.3 frames/s. Fractional
packet scheduling distributes that rate across USB output blocks instead of
repeating one feedback byte across a whole block. The resampler follows the
transmitted rate, with a bounded queue-occupancy trim bridging to CoreAudio's
44.1 kHz stream. The feedback reader and packet layout follow Gregory Senay's
hardware-verified findings. Use `ADAPTIVE_CLOCK=0` only for diagnostic builds.

During October 6, 2026 hardware testing, logs from the fixed-rate build showed
the device feedback and host output diverging while the reported audio hiss
was present. Adaptive clocking is now the default so packet cadence follows
the NS6 feedback; listening tests are ongoing.

The former Numark package included an Intel-only Ploytec kext. This project
does not load or depend on it.

## Audio glitch diagnostics

The USB worker periodically logs transfer health, queued frames, underrun event
and frame counts, largest underrun burst, isochronous packet errors, short
output packets, and USB callback cadence and work duration. CoreAudio
`WriteMix` calls are also measured per client: mean frames per callback, mean
and maximum callback gap, maximum enqueue duration, and the number of calls
whose enqueue work exceeded that callback's audio period. Underruns and packet
faults also produce rate-limited detail messages when they occur. CoreAudio
queue rejections are logged separately.

When a USB output block is short, the worker fades only the missing tail from
the last valid sample toward silence. It does not start a protective rebuffer
or fade in merely because the queue drops below a low-water threshold.
After a glitch, collect the matching system log entries with:

```sh
log show --last 2h --style compact --predicate 'eventMessage CONTAINS[c] "Numark NS6"'
```

Record the glitch time as well; the timestamps let us compare the audio capture
with the driver's queue and USB counters. The diagnostic reader samples USB
feedback endpoint `0x81` on interface 1. The logs distinguish the raw rolling
feedback estimate from the bounded rate actually sent to the device, and retain
requested-versus-sent frame debt as a diagnostic.
When a USB completion callback is delayed by more than 20 ms, the driver also
logs the per-frame USB timestamps. `newest age` compares the last frame's
monotonic timestamp with callback delivery; this distinguishes late callback
delivery from a gap in frame processing.

## Experimental persistent USB owner (0.2.177)

The experimental package separates the persistent USB owner from user-session
MIDI. A system LaunchDaemon owns the NS6 USB transport and publishes the Mach
service. CoreAudio hosts the HAL plug-in in a private `pid/coreaudiod` launchd
domain, so a per-user LaunchAgent's Mach service is not visible to it; the
system service is visible across that boundary. The HAL connects through Mach
IPC and maps shared output and capture rings. `DoIOOperation(WriteMix)` and
`ReadInput` access those rings without USB access, Mach calls, allocation, or
waits. A separate per-user LaunchAgent creates the CoreMIDI endpoints and
bridges MIDI over the existing local UDP transport; it must stay in the logged-in
user session because the system daemon cannot connect to `midiserver`.

The HAL `StartIO`/`StopIO` path notifies the service outside the real-time
callback. The service keeps the USB owner alive across client-flow changes and
resumes the output after USB recovery when an audio client is still active.
The service accepts Mach clients only from `_coreaudiod` and root (the
installer probe). USB behavior across sleep, wake, and reconnection still
needs hardware testing.

This build also monitors the Mach service from a non-realtime HAL thread and
reconnects the shared-memory session after a service restart. HAL callbacks pin
the published mapping while using it, so the monitor can retire an old mapping
without unmapping memory still in use. Installation verifies the Mach protocol
with a small probe. The rollback snapshot includes the HAL, Status app, MIDI
LaunchAgent, and device-service LaunchDaemon. These paths still need hardware
acceptance.

## Build and install

Connect and power on the NS6, then run:

```sh
cd coreaudio
make
make usb-test
make install
```

`make usb-test` sends a 440 Hz tone through the same queue, conversion, and USB
worker used by the HAL plug-in. After installation, select **Numark NS6** in
System Settings > Sound > Output. The installed driver is arm64 and is kept
separate from Numark's old Intel-only bundle.

## Installer package

Build the experimental persistent-owner package with:

```sh
make package
```

The package is written to
`build-experimental/dist-0.2.177/NumarkNS6-0.2.177-experimental-persistent-owner.pkg`.
It is not the published stable package and has not completed hardware
acceptance. The installer snapshots the existing HAL, Status app, LaunchAgents,
and LaunchDaemons so the bundled rollback command can restore the prior
installation. It probes the live Mach protocol before restarting CoreAudio.
Review the package before installing;
installation requires administrator authorization and restarts CoreAudio.
For installation from Terminal:

```sh
sudo installer -pkg build-experimental/dist-0.2.176/NumarkNS6-0.2.176-experimental-persistent-owner.pkg -target /
```

The package installs the arm64 HAL bundle, **Numark NS6 Status**, the system
USB device-service LaunchDaemon, and the user-session MIDI LaunchAgent. It restarts CoreAudio and starts the
service in the system launchd domain. The package is locally built and
unsigned for distribution; it does not include Numark's legacy Intel kext.

To remove this build:

```sh
make uninstall
```

This invokes the rollback script from the installed Status app and restores the
saved HAL, Status app, LaunchAgents, and LaunchDaemons. The rollback snapshot is kept under
`/Library/Application Support/Numark NS6/rollback-previous`.
