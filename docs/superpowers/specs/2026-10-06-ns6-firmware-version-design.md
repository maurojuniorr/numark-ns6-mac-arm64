# NS6 Firmware Version in Status

## Goal

Show the firmware version reported by the connected Numark NS6 in NS6 Status, without attempting to update firmware.

## Evidence and current behavior

Hopper analysis of the original Ploytec `NumarkNS6Audio` x86_64 kext found `PGDevice::scanFirmwareVersion`. It issues a USB control request with request code `0x56`, reads eight bytes, and packs returned bytes into a version value. This is static evidence that the original driver queries a version; it does not establish the exact encoding for every NS6 firmware revision.

The current macOS driver already owns the NS6 USB interface while audio/MIDI functions are active. The status app should not open a second USB handle to query firmware, since that can conflict with the driver.

## Design

The driver performs the version query through its existing USB ownership/lifecycle, decodes only the version fields supported by evidence, and publishes a read-only firmware-version property through the driver's existing status mechanism. NS6 Status displays that value and reports “Unavailable” when the device is disconnected, the request fails, or the response is malformed. The implementation must not enter firmware-update mode or write firmware data.

## Requirements

- Query with USB request `0x56` only after confirming the required device/interface state.
- Treat USB failure, short response, and malformed fields as unavailable; never display a fabricated version.
- Keep the USB query out of the audio real-time callback.
- Expose the value through the existing driver-to-status path; do not open a competing USB connection from the app.
- Add tests for valid response decoding and invalid/short responses.
- Update status UI and documentation to distinguish firmware version from driver version.

## Open implementation detail

Before coding the decoder, confirm byte order and field layout from the kext's published version property and/or observed query response. If Hopper cannot establish it, add a read-only probe on the existing driver-owned path and validate against the user's NS6 before displaying a formatted version.

## Out of scope

- Firmware update, flashing, or entering update mode.
- Changing audio or MIDI behavior.
- Claiming compatibility with firmware revisions that have not been observed.
