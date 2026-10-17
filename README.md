# 🎧 Numark NS6 driver for Apple Silicon macOS

An independent driver project to keep the original Numark NS6 usable on modern
Apple Silicon Macs. It provides four-channel audio playback and stereo capture
through Core Audio, MIDI input and LED output through CoreMIDI, and a small
status app for the device and active audio stream.

The macOS implementation builds on the protocol and initialization work in
[our Linux NS6 driver](https://github.com/maurojuniorr/numark-ns6-linux). That
Linux project is also still in progress. This repository adapts that shared
hardware knowledge to macOS; it is not a port of Numark's discontinued driver
and is not an official Numark or Ploytec product.

> **1.0 candidate: package version 0.2.140.** It builds on the 0.2.138 audio
> and MIDI baseline, adds the stereo capture path, and fixes MIDI Bridge
> replacement and startup during package upgrades. It is a candidate, not a
> formal 1.0 release. The `.pkg` has not yet been attached to a GitHub Release.

## ✨ What's in this candidate

- Adds a two-channel, 44.1 kHz capture stream decoded from the NS6 USB input
  endpoint while an application requests input.
- The installer upgrades the HAL driver and Status app together, unloads an old
  MIDI Bridge before replacing its app bundle, and starts the installed bridge
  after the upgrade.
- Adds regression checks for capture decoding, its audio ring, USB endpoint
  configuration, and package contents/lifecycle.

## 👨‍🔧 About the project

Created and maintained by **Mauro Junior** ([maurojuniorr](https://github.com/maurojuniorr)),
DJ and software developer working to bring the original NS6 back to life on
current Macs. The original Mac driver depended on an Intel-only Ploytec kernel
extension; this project uses a userspace Core Audio HAL driver and does not
require that legacy extension or changes to SIP.

The target is the original USB device, **VID `15e4`, PID `0079`**, on Apple
Silicon Macs running macOS 15 or later.

## ✅ What works

- **Four-channel audio output:** 44.1 kHz, packed 24-bit audio. Output channels
  1–2 carry Master and channels 3–4 carry Headphones, so DJ software can route
  both mixes independently.
- **MIDI controls and LEDs:** the driver reads the NS6's packed USB MIDI messages and
  publishes a **Numark NS6** CoreMIDI input. DJ software can send MIDI back to
  the controller's LEDs through the matching CoreMIDI output. The bridge is
  installed with the Status app and starts in the logged-in user session.
- **Adaptive USB audio clock:** endpoint `0x81` feedback drives the five/six
  frame packet schedule for an exact 44.1 kHz average. The output path has
  played multi-hour sessions cleanly on the maintainer's M1 Pro/Sequoia setup;
  the recent 0.2.138 baseline is the known stable output build.
- **Two-channel audio input:** the candidate exposes the NS6 capture stream at
  44.1 kHz. Input capture worked in an earlier hardware test; this integration
  still needs a fresh test alongside playback and MIDI before calling it a
  stable 1.0 feature.
- **NS6 Status app:** reports USB connection, the installed driver's version,
  firmware, audio format, and the CoreAudio buffer cycle observed for the app
  using the active audio flow.
- **Audio recovery and diagnostics:** the USB transport can recover from
  transient device/interface loss and logs transfer health, queue underruns,
  packet errors, and feedback-endpoint readings for troubleshooting.

**Mixxx controls its own CoreAudio buffer period.** The HAL driver now accepts
the frame size requested by the client and returns that active value through
CoreAudio, instead of reporting a fixed 96-frame period. NS6 Status reports the
observed cycle size and corresponding latency for the current flow; during
hardware testing, that latency matched Mixxx's report.

## 🏗️ How the driver is put together

The Core Audio HAL driver owns the NS6 USB session and sends playback over its
isochronous audio endpoint. Audio is queued outside Core Audio's real-time
callback, converted to the packed format used by the controller, and sent in
whole-frame USB packets. MIDI is carried over the same USB session and
forwarded to a companion process, which publishes the CoreMIDI ports in the
logged-in user session. This lets audio and MIDI share the device without two
processes competing to open it.

The USB audio schedule uses whole five- and six-frame packets, with endpoint
`0x81` feedback selecting the frame pattern for each millisecond. Adaptive
clocking is enabled by default in source builds and release packages; the
resampler follows the transmitted rate while a bounded queue trim handles
residual host/device drift. The driver logs USB callback cadence and work
duration, CoreAudio `WriteMix` cadence and enqueue duration, queue depth,
packet/transfer errors, and recovery events.

## ⚠️ What remains before a stable 1.0 release

- **Revalidate input on this candidate.** Run microphone and line capture, then
  test capture with playback and MIDI active at the same time. The capture code
  is implemented and has prior hardware evidence, but this exact integration
  has not yet had that full test.
- **Publish the installer.** Commit `a60747b` and this candidate's source are on
  `main`; the 0.2.140 `.pkg` is currently a local build and is not attached to a
  GitHub Release. Build it with the command below before distributing it.
- **Broader hardware coverage is still needed.** Testing on other Apple Silicon
  Macs, macOS releases, DJ applications, and longer sessions remains open.
- NS6 Status reads a firmware-version property populated by a read-only
  vendor request. The connected NS6 returned `31 01 03 02 02`, matching
  `1.0.3 (K1)` from the original Windows panel; other firmware variants have
  not been validated.
- This project targets the original NS6 on Apple Silicon macOS 15 or later. The
  NS6II and Intel Macs are not supported by this build.
- The separate `tools/ns6-probe` program only reads USB descriptors. It is a
  development diagnostic, is not required by the driver, and is not included
  in the installer.
- The installer package is unsigned and not notarized. macOS may require you
  to approve or explicitly open it.

## 📥 Install

The 0.2.140 candidate `.pkg` is not published as a GitHub Release asset yet.
Build it from this repository on an Apple Silicon Mac with Xcode Command Line
Tools:

```sh
cd coreaudio
make package VERSION=0.2.140
```

This creates `coreaudio/dist/NumarkNS6-0.2.140.pkg` from the repository root.
Install it with macOS Installer; the installer restarts CoreAudio and briefly
interrupts system audio. It installs the HAL driver, **Numark NS6 Status**, and
the CoreMIDI bridge. Administrator authorization is required. Connect and
power on the NS6, then select **Numark NS6** in System Settings → Sound or in
your DJ application's audio preferences.

In Mixxx, select **Numark NS6** for both the audio device and the controller's
MIDI input/output. Route Master to channels 1–2 and Headphones to channels
3–4. The MIDI bridge starts automatically after installation when you log in.

## 🧪 Build and test from source

Build the driver, status app, and MIDI bridge with Xcode Command Line Tools on
an Apple Silicon Mac:

```sh
cd coreaudio
make
make midi-parser-test
```

The adaptive-clock build and its regression tests can be checked with:

```sh
make ADAPTIVE_CLOCK=1 clock-control-test
make clock-control-fixed-test
make usb-work-interval-regression-test
```

To create a local installer package and run the package checks:

```sh
make package
```

The package is written to `coreaudio/dist/`. For USB descriptor diagnostics,
build and run the read-only probe separately:

```sh
cd tools
make ns6-probe
./ns6-probe
```

The probe lists the NS6 USB configuration and endpoints. It does not claim an
interface, initialize the controller, or send audio or MIDI.

## 🤝 Technical heritage and acknowledgements

This macOS driver continues the work in
[maurojuniorr/numark-ns6-linux](https://github.com/maurojuniorr/numark-ns6-linux),
which established the NS6 vendor activation, SysEx initialization sequence,
initial controller state, and the early USB/audio/MIDI investigation. The
Linux kernel driver remains incomplete; the macOS implementation is a separate
Core Audio/CoreMIDI adaptation of that shared work.

**Honorable mention — [Gregory Senay](https://github.com/GregorySenay).** His
independent NS6 protocol research and Apple Silicon hardware testing contributed
important findings to this effort. In particular, his analysis helped verify
the `0x50`/`0x51`/`0x60` initialization exchange, identify the `0x81` feedback
endpoint's 44/45-frame reports, and establish why audio packets must carry whole
frames in a five/six-frame cadence for an exact 44.1 kHz average. His related
Linux kernel proposal and test notes are in
[PR #4](https://github.com/maurojuniorr/numark-ns6-linux/pull/4); his separate
macOS research is at [GregorySenay/ns6-macos](https://github.com/GregorySenay/ns6-macos).
This project credits and builds on those findings while maintaining its own
implementation and test results.

## 🛠️ Help improve it

Testing on another Apple Silicon Mac, macOS release, or DJ application is
valuable. Please include the macOS version, driver version, application and
audio settings, and the approximate time of any disconnect or audio artifact
when opening an issue. Contributions to the driver, diagnostics, and
documentation are welcome.
