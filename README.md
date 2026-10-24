# Numark NS6 for Apple Silicon macOS

The active modular driver and release-candidate source are in [`numark-ns6/`](numark-ns6/README.md). The older `coreaudio/` tree is retained in Git history; use `numark-ns6/` for current builds and packages.

## 1.0.0 release candidate

The `v1.0.0-rc.1` candidate packages the hardware-tested modular implementation. It provides separate **Numark NS6 Output** and **Numark NS6 Input** CoreAudio devices, bidirectional MIDI through the session bridge, the Status app, and automatic activation/recovery after reconnect. The candidate's resolved issues and hardware validation are documented in [`numark-ns6/docs/releases/1.0.0-rc.1.md`](numark-ns6/docs/releases/1.0.0-rc.1.md).

Build the installer on an Apple Silicon Mac with Xcode Command Line Tools:

```sh
./numark-ns6/tools/build-package.sh /private/tmp/ns6-1.0.0-rc-build \
  /private/tmp/NumarkNS6-1.0.0-rc.1.pkg
```

The package installs the HAL driver, MIDI bridge, LaunchAgent, and Numark NS6 Status app. Installation requires administrator authorization and briefly restarts CoreAudio. The package is unsigned and not notarized, so macOS may require approval before opening it.

## Validation scope

The current hardware test confirmed clean Mixxx playback while MIDI controls and direct NS6 Input capture in TikTok LIVE Studio were active. OBS capture and recovery after USB reconnect were also confirmed. Testing on additional Macs, macOS releases, and longer live sessions remains open. This is a release candidate, not a final 1.0 release.
