# Numark NS6 macOS Driver

An independent CoreAudio and CoreMIDI driver for the original Numark NS6 on Apple Silicon Macs. The goal is to keep this controller usable with current macOS DJ and streaming software, without relying on the discontinued Intel-only Ploytec kernel extension.

This project is not an official Numark product. It targets the original NS6 (USB VID `15e4`, PID `0079`), not the NS6II. Development is based on protocol research, examination of the original driver, and testing with a physical controller.

## Current release: 1.0.0

The first 1.0 release packages the modular driver, MIDI bridge, automatic activation and recovery, and Status app. The installer is available from [GitHub Releases](https://github.com/maurojuniorr/numark-ns6-mac-arm64/releases/tag/v1.0.0).

## What works

- **Audio output:** four-channel Float32 playback at 44.1 kHz. Channels 1–2 carry Master and channels 3–4 carry Headphones, allowing DJ software to route both mixes independently.
- **Audio input:** a separate stereo CoreAudio device, **Numark NS6 Input**, exposes the controller's hardware master/record return. The NS6 performs the mix in hardware; the driver does not copy its software output into the input. OBS and TikTok LIVE Studio have captured this input while Mixxx continued playback.
- **MIDI:** bidirectional controller messages and LED output through CoreMIDI, retaining the device identity **Numark NS6** used by existing mappings.
- **Automatic activation and reconnect:** the session bridge keeps the output active while the controller is connected and reattaches after USB reconnection. Hardware testing confirmed audio and MIDI recovery without restarting Mixxx or the capture app.
- **Status app:** reports firmware and active CoreAudio clients. The installer installs or updates the app along with the driver and bridge.
- **Install, upgrade, and uninstall:** package scripts remove conflicting legacy components. The uninstaller removes driver components and receipts while preserving logs and user preferences.

## Problems resolved

- **TikTok LIVE Studio showed no input level.** The driver now advertises a timestamp period accepted by the app; the NS6 Input meter moves while Mixxx playback and MIDI remain active.
- **Input pops despite clean output.** Captures showed malformed USB sample lanes even though USB error, dropped-frame, and ring-starvation counters remained at zero. The input reader now keeps eight bulk-IN transfers queued to provide more scheduling margin. A 5.5-minute direct capture and simultaneous Mixxx/TikTok test had no sample jumps or audible pops. Audio samples are not altered to hide transport faults.
- **Audio and MIDI did not reliably resume after USB reconnect or package update.** The bridge now reattaches the CoreAudio keepalive and MIDI session after the NS6 returns.
- **Legacy helper conflicts.** Package upgrade hooks stop the old service and remove the conflicting driver before restarting CoreAudio.
- **MIDI responsiveness during playback.** The modular MIDI bridge and USB scheduling were tested with Mixxx playing; the controls remained responsive during the successful simultaneous input/output test.

See the detailed [1.0.0 release notes](numark-ns6/docs/releases/1.0.0.md) for validation details and known limits.

## Current limits

- The extended no-pop hardware capture lasted about 5.5 minutes. Longer live streams and testing on other Macs and macOS versions are still needed to establish long-term reliability.
- The stereo input is the NS6 hardware master/record mix; it does not expose separate microphone and line channels.
- MIDI SysEx and system-common messages are not forwarded. USB disconnection can also discard MIDI messages already in flight.
- The package is unsigned and not notarized. macOS may require approval before opening it.

## Install

Download **NumarkNS6-1.0.0.pkg** from [the 1.0.0 release page](https://github.com/maurojuniorr/numark-ns6-mac-arm64/releases/tag/v1.0.0), open it, and follow Installer. Administrator authorization is required; installation briefly restarts CoreAudio.

After installation, connect and power on the NS6. In Mixxx, select **Numark NS6 Output** as the sound device and route Master to channels 1–2 and Headphones to channels 3–4. Select **Numark NS6** for the controller's MIDI input and output. For recording or streaming, select **Numark NS6 Input** as the audio input.

## Build and test from source

Build on an Apple Silicon Mac with Xcode Command Line Tools:

```sh
./numark-ns6/tools/build-package.sh /private/tmp/ns6-build \
  /private/tmp/NumarkNS6-local.pkg
```

The packaging script builds the HAL driver, MIDI bridge, Status app, and installer, then runs the project's 14 automated tests. To build and run the test suite without packaging:

```sh
cmake -S numark-ns6 -B /private/tmp/ns6-build
cmake --build /private/tmp/ns6-build
ctest --test-dir /private/tmp/ns6-build --output-on-failure
```

To remove the installed driver before testing another package:

```sh
sudo ./numark-ns6/tools/uninstall-driver.sh
```

The uninstaller asks for confirmation and restarts CoreAudio. It preserves logs and user preferences.

## Project layout

- `numark-ns6/` — current modular CoreAudio driver, MIDI bridge, Status app, package scripts, tests, and release notes.
- `coreaudio/` — earlier implementation and diagnostics retained for historical reference; use `numark-ns6/` for current builds and packages.
- `docs/` — hardware research, diagnostic records, and design notes.

## Contributing and reporting issues

Testing on another Apple Silicon Mac, macOS release, DJ application, or streaming app is welcome. For a useful bug report, include the driver version, macOS version, app and audio settings, whether input/output/MIDI were active together, the approximate time of the issue, and relevant driver logs. Contributions to the driver, tests, and documentation are welcome.
