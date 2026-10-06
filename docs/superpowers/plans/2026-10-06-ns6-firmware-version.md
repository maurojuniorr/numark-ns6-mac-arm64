# NS6 Firmware Version in Status Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Display the connected NS6 firmware version in NS6 Status using the driver's existing USB ownership.

**Architecture:** Decode the eight-byte response to USB request `0x56` in a pure/testable helper. Query only on the USB worker outside real-time audio, publish a read-only custom HAL property, and let the status app render the value or “Unavailable”.

**Tech Stack:** C11, IOKit USB, CoreAudio AudioServerPlugIn custom properties, Swift/AppKit, Makefile tests.

**Spec:** `docs/superpowers/specs/2026-10-06-ns6-firmware-version-design.md`

## Global Constraints

- Read firmware metadata only; never enter firmware-update mode or write firmware data.
- Do not open a second USB handle from NS6 Status.
- Never issue USB requests from the CoreAudio real-time callback.
- Report unavailable for failed, short, or unverified responses.

## Review Focus

- Malformed or short control-transfer response: decoder rejects it and UI says unavailable.
- Device disconnect during or after query: property becomes unavailable without stalling USB/audio.
- Unknown version encoding: do not format a guessed version; confirm encoding before publishing.
- HAL property polling: status app reads the driver's property rather than querying USB itself.
- Driver is idle at startup: query after USB transport initialization and notify property changes when value changes.

---

### Task 1: Verify firmware response encoding and add decoder tests

**Files:**
- Create: `coreaudio/NS6FirmwareVersion.h`
- Create: `coreaudio/NS6FirmwareVersionTest.c`
- Modify: `coreaudio/Makefile`

**Interfaces:**
- Produces `bool ns6_firmware_version_decode(const uint8_t response[8], size_t length, char output[32])`.
- A valid response formats only when the version byte layout is confirmed from `scanFirmwareVersion` and the kext's exposed version representation; otherwise preserve raw bytes as unavailable pending hardware confirmation.

- [ ] **Step 1: Write decoder tests** for the confirmed firmware response, zero/unsupported response, and lengths below eight bytes.
- [ ] **Step 2: Run the test** with `make -C coreaudio firmware-version-test`; verify the new test fails because the decoder is absent.
- [ ] **Step 3: Inspect Hopper pseudocode/callers** for `PGDevice::scanFirmwareVersion` and determine how packed field `r15[0x623]` is exposed; record a real response from the existing device-owned USB path if static analysis does not prove formatting.
- [ ] **Step 4: Implement the pure decoder** only for the proven byte format; return false for unproven or malformed layouts.
- [ ] **Step 5: Run `make -C coreaudio firmware-version-test`** and verify all decode/reject cases pass.

### Task 2: Query through USB worker and publish firmware property

**Files:**
- Modify: `coreaudio/NS6USB.h`
- Modify: `coreaudio/NS6USB.c`
- Modify: `coreaudio/NS6HAL.c`
- Modify: `coreaudio/Makefile`

**Interfaces:**
- USB worker exposes a thread-safe accessor for the last verified version string and schedules request `0x56` only while it owns the configured NS6 control interface.
- HAL publishes read-only custom property selector `'ns6v'` as a CFString, using “Unavailable” when no valid value exists.

- [ ] **Step 1: Add a HAL property test/helper test** asserting the property is read-only and the empty/disconnected state returns unavailable.
- [ ] **Step 2: Run targeted tests** and confirm they fail before implementation.
- [ ] **Step 3: Add the request `0x56`** in the existing USB worker/lifecycle after interface configuration; enforce an 8-byte response and store only a decoded verified version.
- [ ] **Step 4: Add the read-only custom property** and notify CoreAudio when the value changes or the device disconnects.
- [ ] **Step 5: Run `make -C coreaudio test`** and inspect logs to confirm the request is outside realtime callbacks.

### Task 3: Display firmware version in NS6 Status

**Files:**
- Modify: `coreaudio/status-app/NS6Status.swift`
- Modify: `coreaudio/README.md`

**Interfaces:**
- Status reads the custom property `'ns6v'` from the NS6 AudioObject; it does not call IOKit USB APIs.

- [ ] **Step 1: Add a parsing/display test or testable formatter** for valid and unavailable property values.
- [ ] **Step 2: Run the formatter test** and verify unavailable/invalid states fail as expected before implementation.
- [ ] **Step 3: Add a Firmware Version row** distinct from Driver Version and refresh it on device/property changes.
- [ ] **Step 4: Document the read-only firmware query** and its unavailable behavior.
- [ ] **Step 5: Build the status app and full package**; verify with a connected NS6 that the displayed version matches the original Ploytec report or the driver's direct response.
