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

> **Current tested build: 0.2.127.** During a 90-minute playback session on
> an M1 Pro Mac running macOS Sequoia, the maintainer reported no audible
> clicks or glitches. The adaptive clock stayed within 0.175 Hz of 44.1 kHz.
> See the [0.2.127 test record](docs/research/2026-10-13-audio-stability-0.2.127.md)
> for diagnostic details and the isolated feedback-recovery event observed in
> that session. Testing with other Macs, macOS releases, and audio applications
> is still welcome.

## ✨ What's new in 0.2.127

- Feedback diagnostics now distinguish short packets, out-of-range values,
  transport read errors, stale-feedback resets, and clock relock events. This
  helps identify recovery behavior without changing the adaptive clock's
  nominal 44.1 kHz fallback.
- The 0.2.127 test run sustained playback for 90 minutes with no audible
  artifacts reported by the maintainer. The feedback endpoint produced one
  brief recovery episode; underrun counters then remained unchanged for the
  rest of the observed run.

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

The experimental 0.2.174 build moves USB ownership into a persistent,
per-user LaunchAgent. The Core Audio HAL plug-in and MIDI bridge communicate
with that owner through Mach IPC and shared-memory audio rings. The real-time
HAL callback only copies PCM to or from those rings; USB and MIDI operations
stay in the service, so replacing or restarting the HAL helper does not create
a second process that claims the NS6 interfaces.

The USB audio schedule uses whole five- and six-frame packets, with endpoint
`0x81` feedback selecting the frame pattern for each millisecond. Adaptive
clocking is enabled by default in source builds and release packages; the
resampler follows the transmitted rate while a bounded queue trim handles
residual host/device drift. The driver logs USB callback cadence and work
duration, CoreAudio `WriteMix` cadence and enqueue duration, queue depth,
packet/transfer errors, and recovery events.

## ⚠️ Current limitations

- **Persistent USB ownership is experimental.** The 0.2.174 package has passed
  local unit, IPC, HAL-boundary, packaging, and arm64 build checks, but has not
  yet completed hardware acceptance. The published stable release remains the
  safer choice until the experimental package is installed and exercised.
- **Audio capture through the persistent service is not hardware-validated.**
  The service now routes the capture stream through shared memory, but
  simultaneous input, output, MIDI, disconnect/reconnect, and helper-restart
  behavior still need testing on the NS6.
- **Hardware coverage is limited.** The current release has been stable in
  recent sessions on the maintainer's M1 Pro/Sequoia setup; testing on other
  systems and longer sessions is still needed to characterize rare failures.
- NS6 Status reads a firmware-version property populated by a read-only
  vendor request. The connected NS6 returned `31 01 03 02 02`, matching
  `1.0.3 (K1)` from the original Windows panel; other firmware variants have
  not been validated.
- The separate `tools/ns6-probe` program only reads USB descriptors. It is a
  development diagnostic, is not required by the driver, and is not included
  in the installer.
- The installer package is unsigned and not notarized. macOS may require you
  to approve or explicitly open it.

## 📥 Install

Download the latest stable **NumarkNS6 `.pkg`** from
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

To build the experimental persistent-owner package locally:

```sh
make experimental-package
```

The 0.2.174 package is written to
`coreaudio/build-experimental/dist-0.2.174/` and is deliberately separate from the
published stable release. Do not treat it as a stable release until hardware
acceptance is complete. For USB descriptor diagnostics,
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
