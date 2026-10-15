# Persistent USB owner 0.2.173 — hardware acceptance record

**Status: pending hardware validation.** The experimental package was built and
validated locally on 2026-10-15. It has not been installed, so this record does
not claim that the persistent owner is stable on a connected NS6.

## Build artifact

- Package: `coreaudio/build-experimental/dist/NumarkNS6-0.2.173-experimental-persistent-owner.pkg`
- Target: arm64, macOS 15+
- Package identifier: `io.github.maurojuniorr.numark-ns6.experimental`
- SHA-256: `c4ea5656a3bb23fc57300732434753d713356ad5c3c1df7c7815db8be1bbe143`

## Local verification

- `make -C coreaudio test device-ipc-test device-owner-test shared-memory-test capture-test hal-service-boundary-test service-lifecycle-test experimental-package` — passed.
- The HAL callback boundary test confirms `WriteMix` and `ReadInput` use shared rings without USB, Mach IPC, allocation, or waits.
- Mach IPC, shared-memory, device-owner, capture decoder/ring, MIDI lifecycle/recovery, clock, and USB regression tests passed.
- HAL and Status app bundles were signed locally and passed `codesign --verify --strict`.
- The package contains the arm64 HAL, Status app, device service, LaunchAgent, preinstall/postinstall scripts, and rollback command. Both bundle versions report 0.2.173.
- The lifecycle test checks that the old HAL/bridge are saved before removal, the new service is running before CoreAudio is restarted, and rollback restores the saved HAL before restarting CoreAudio.

## Hardware acceptance still required

After the user approves installation, verify on the M1 Pro / Sequoia NS6 setup:

1. Open, pause, and close Opera and Mixxx at least 20 times; check for no
   `kIOReturnExclusiveAccess`, pipe stalls, or second USB owner.
2. Play audio for at least 60 minutes and compare audible glitches and Mixxx
   underrun counters before/after.
3. Test simultaneous output, physical input capture, and MIDI controls/LEDs.
4. Disconnect/reconnect the controller and restart the HAL helper while audio
   is active; confirm the service keeps one USB owner and audio recovers without
   a physical power cycle.
5. Record service PID, interface-claim logs, stream period, queue/underrun
   counters, USB errors, and LED behavior for each transition.

Until those checks pass, 0.2.173 remains experimental and is not a stable
release candidate.
