# 🎧 Numark NS6 driver for Apple Silicon macOS

An independent driver project to keep the original Numark NS6 usable on modern
Apple Silicon Macs. It provides four-channel audio playback through Core Audio,
MIDI input and LED output through CoreMIDI, and a small status app for the
device and active audio stream.

The macOS implementation builds on the protocol and initialization work in
[our Linux NS6 driver](https://github.com/maurojuniorr/numark-ns6-linux). That
Linux project is also still in progress. This repository adapts that shared
hardware knowledge to macOS; it is not a port of Numark's discontinued driver
and is not an official Numark or Ploytec product.

> **Project status: experimental / alpha.** The driver has been exercised on
> original NS6 hardware with Apple Silicon and macOS Sequoia, including
> extended audio sessions. Compatibility and long-term behavior still need
> testing across more systems and applications. Use it for testing and report
> issues with logs and the macOS version.

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
- **MIDI controls:** the driver reads the NS6's packed USB MIDI messages and
  publishes a **Numark NS6** CoreMIDI input. DJ software can send MIDI back to
  the controller's LEDs through the matching CoreMIDI output.
- **NS6 Status app:** reports USB connection and audio format details,
  and the active audio flow when available. It also lets you choose the USB
  startup pre-buffer and apply the change by restarting the audio transport.
- **Audio recovery and diagnostics:** the USB transport can recover from
  transient device/interface loss and logs transfer health, queue underruns,
  packet errors, and feedback-endpoint readings for troubleshooting.

**Mixxx controls its own CoreAudio buffer period.** The HAL driver now accepts
the frame size requested by the client and returns that active value through
CoreAudio, instead of reporting a fixed 96-frame period. NS6 Status reports the
active flow's frame count and corresponding latency; during hardware testing,
that latency matched Mixxx's report. The Status app's USB startup pre-buffer is
a separate driver setting and does not override Mixxx's choice.

## 🏗️ How the driver is put together

The Core Audio HAL driver owns the NS6 USB session and sends playback over its
isochronous audio endpoint. Audio is queued outside Core Audio's real-time
callback, converted to the packed format used by the controller, and sent in
whole-frame USB packets. MIDI is carried over the same USB session and
forwarded to a companion process, which publishes the CoreMIDI ports in the
logged-in user session. This lets audio and MIDI share the device without two
processes competing to open it.

The USB audio schedule uses a fractional frame accumulator to send whole
five- and six-frame packets at the NS6's 44.1 kHz rate. The adaptive build reads
the controller's `0x81` feedback endpoint and adjusts the source-frame step
against the USB frame schedule while keeping output within 44,000–44,100.3
frames per second. Build it with `ADAPTIVE_CLOCK=1`; it remains an explicit
experimental option while it is tested with more applications and sessions.

During the October 6, 2026 follow-up test on build 0.2.102, Mauro reported no
audible clicks or pops. Recent health logs showed zero driver underruns, USB
transfer errors, packet errors, or short packets; the requested-versus-sent
frame debt stayed bounded. Callback cadence still reached about 17 ms, and the
feedback reader had four read errors in the observed run, so longer listening
tests remain important. These results describe the current test, not a guarantee
that artifacts can never return. The driver logs callback cadence and work
duration, queue depth, frame timestamps on delayed completions, packet/transfer
errors, and recovery events to investigate future glitches.

## ⚠️ Current limitations

- **Audio capture is not implemented** as a macOS input device. The NS6's two
  hardware inputs are not available to applications through this driver.
- **Long-term audio stability is still under test.** The current test has been
  clean by ear and the driver counters are clear, but callback timing varies
  and the feedback reader recorded occasional errors.
- The firmware version is not queried from the controller yet; NS6 Status
  reports it as unavailable.
- The separate `tools/ns6-probe` program only reads USB descriptors. It is a
  development diagnostic, is not required by the driver, and is not included
  in the installer.
- The installer package is unsigned and not notarized. macOS may require you
  to approve or explicitly open it.

## 📥 Install

Download the latest **NumarkNS6 `.pkg`** from
[GitHub Releases](https://github.com/maurojuniorr/numark-ns6-mac-arm64/releases).
Open the package and follow Installer. It installs the Core Audio driver,
**Numark NS6 Status**, and the CoreMIDI bridge. Administrator authorization is
required. Connect and power on the NS6, then select **Numark NS6** in System
Settings → Sound → Output or in your DJ application's audio preferences.

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

To create a local installer package:

```sh
make ADAPTIVE_CLOCK=1 package VERSION=0.2.102
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
