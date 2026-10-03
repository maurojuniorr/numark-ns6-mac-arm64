# CoreAudio HAL device

Created and maintained by **Mauro Junior** ([maurojuniorr](https://github.com/maurojuniorr)).

This directory is the macOS output-device layer for the original Numark NS6.

The USB transport has already been validated on Apple Silicon through
`macos/tools/ns6-native-tone.c`: it claims interface 0, selects alternate
setting 1, uses `LowLatencyWriteIsochPipeAsync`, and produces a clean 44.1 kHz
test tone through the NS6.

The HAL plug-in publishes one 4-channel, 44.1 kHz Float32 output device. Its
NS6 Status app selects the CoreAudio buffer size live among 49, 128, 192, 256, 512, and
1024 frames, matching the choices shown in the original Windows NS6 panel. A
change is requested through CoreAudio's device-configuration mechanism so
clients can stop and restart their I/O around the new period. At 44.1 kHz,
those sizes correspond to approximately 1.1, 2.9, 4.4, 5.8, 11.6, and
23.2 ms per buffer. Its
worker converts the CoreAudio mix to the packed 24-bit format used by the NS6.
Channels 1/2 feed Master and channels 3/4 feed Headphones independently, so
DJ software can route its main mix and cue mix separately.

At stream start, the worker waits briefly for 2048 PCM frames before submitting
the first eight USB transfers. This prevents the device from alternating
between empty packets and live audio while CoreAudio is filling its first
buffers.

During playback, the USB worker uses a fixed 5/6-frame cadence. The 441/80
fractional accumulator emits only complete 12-byte frames and averages exactly
44,100 frames/s; changing packet sizes from the host queue is audible on the
NS6.

The former Numark package included an Intel-only Ploytec kext. This project
does not load or depend on it.

## Audio glitch diagnostics

The USB worker periodically logs transfer health, queued frames, underrun event
and frame counts, largest underrun burst, isochronous packet errors, and short
output packets. Underruns and packet faults also produce rate-limited detail
messages when they occur. CoreAudio queue rejections are logged separately.
After a glitch, collect the matching system log entries with:

```sh
log show --last 2h --style compact --predicate 'eventMessage CONTAINS[c] "Numark NS6"'
```

Record the glitch time as well; the timestamps let us compare the audio capture
with the driver's queue and USB counters. The diagnostic reader also samples
the optional USB feedback endpoint `0x81` on USB interface 1 and logs one-second
windows of its 44/45-frame reports; it does not use those values to alter playback timing.

## Data path

`DoIOOperation(WriteMix)` copies PCM to `NS6Transport` without allocation or
locking. The USB worker removes packed 24-bit frames from that queue and emits
the 5/6-frame high-speed ISO packet pattern used by the validated native
transport. This separation is required because CoreAudio invokes `DoIOOperation`
on a real-time deadline while USB completion callbacks run on a normal run loop.

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

Build a local macOS installer package with:

```sh
make package
```

The package is written to `dist/NumarkNS6-0.1.0.pkg`. Open it in Finder or
install it from Terminal with:

```sh
sudo installer -pkg dist/NumarkNS6-0.1.0.pkg -target /
```

It installs the arm64 HAL bundle in `/Library/Audio/Plug-Ins/HAL` and restarts
only `coreaudiod`. It also adds **Numark NS6 Status** to Applications. The app
shows live USB connection state, output format, channel count, and the driver
version. Firmware is deliberately shown as not queried until the driver can
read it from the hardware. The package is locally built and unsigned for
distribution; it does not require or include Numark's legacy Intel kext.

To remove this build:

```sh
make uninstall
```
