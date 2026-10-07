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

At stream start, the worker waits briefly for 2048 PCM frames before submitting
the first eight USB transfers. This prevents the device from alternating
between empty packets and live audio while CoreAudio is filling its first
buffers.

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
output packets, callback cadence, and callback work duration. Underruns and
packet faults also produce rate-limited detail messages when they occur.
CoreAudio queue rejections are logged separately.
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

## Data path

`DoIOOperation(WriteMix)` copies PCM to `NS6Transport` without allocation or
locking. The USB worker resamples packed 24-bit frames from that queue by a
fractional source phase, then emits the 5/6-frame high-speed ISO packet pattern
used by the validated native transport. Queue occupancy trims the host/device
clock ratio to keep the bounded FIFO away from starvation and overflow. This
separation is required because CoreAudio invokes `DoIOOperation` on a real-time
deadline while USB completion callbacks run on a normal run loop.

## Build and install

Connect and power on the NS6, then run:

```sh
cd macos/coreaudio
make
make usb-test
make install
```

`make usb-test` sends a 440 Hz tone through the same queue, conversion, and USB
worker used by the HAL plug-in. After installation, select **Numark NS6** in
System Settings > Sound > Output. The installed driver is arm64 and is kept
separate from Numark's old Intel-only bundle.

## Installer package

Build an adaptive macOS installer package with:

```sh
make package
```

The package is written to `dist/NumarkNS6-$(VERSION).pkg`. Open it in Finder or
install it from Terminal with:

```sh
sudo installer -pkg dist/NumarkNS6-0.2.102.pkg -target /
```

It installs the arm64 HAL bundle in `/Library/Audio/Plug-Ins/HAL` and restarts
only `coreaudiod`. It also adds **Numark NS6 Status** to Applications. The app
shows live USB connection state, output format, channel count, and the driver
version. Firmware is read from the NS6 through a vendor request and displayed
in Status (the connected unit reported `1.0.3 (K1)`). The package is locally built and unsigned for
distribution; it does not require or include Numark's legacy Intel kext.

To remove this build:

```sh
make uninstall
```
