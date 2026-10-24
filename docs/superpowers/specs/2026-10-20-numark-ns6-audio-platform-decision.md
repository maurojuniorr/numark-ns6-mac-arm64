# Numark NS6 audio platform decision

**Status:** AudioServerPlugIn + user-space USB selected for the fresh modular implementation. DriverKit is deferred because no signing team/approved entitlements are available.

## Findings

- Host macOS is 15.7.4; Xcode is 26.3 with the macOS 26.2 SDK.
- `xcodebuild -showsdks` lists DriverKit 25.2. `AudioDriverKit.framework` (including `IOUserAudioDriver.h`) and `USBDriverKit.framework` are in `DriverKit25.2.sdk`, not in the macOS SDK.
- A syntax-only compile including `AudioDriverKit/IOUserAudioDriver.h` and `USBDriverKit/IOUSBHostDevice.h` succeeds with `xcrun --sdk driverkit25.2 clang -std=c++17`.
- I/O Registry reports the NS6 as an enumerated `IOUSBHostDevice` from Numark (`0x15e4:0x0079`).
- The read-only descriptor probe requires elevation on this host. As root it confirmed interface 0 alt 1: isochronous OUT `0x02` (156-byte max, interval 1); interface 1 alt 1: isochronous IN `0x81` (64-byte max, interval 4) plus bulk IN `0x86` (audio capture). The MIDI endpoints are bulk IN `0x83` and bulk OUT `0x04`, both 512-byte max, interval 4.
- The passive interface-open probe found interface 0 but `USBInterfaceOpen` returned `kIOReturnExclusiveAccess` (`0xe00002c5`) while the installed 0.1.41 driver was actively using the NS6. This is expected ownership, not a permission failure; no interface was seized.
- The last 30 minutes of 0.1.41 output-health logs contained one anomaly at 20:07:24: 106 full drops/54,272 frames, 572 underrun frames, and queue depth 356. At 20:07:22.038 the kernel logged `hardware connection lost` for `15e4:0079`; it re-enumerated at 20:07:22.046 and the driver restarted transport at 20:07:23.280. The other 1,798 health records had no underrun, short packet, or full drop. The event correlates with USB disconnect/reconnect, not steady-state clock drift.
- `security find-identity -v -p codesigning` reports zero valid signing identities. `systemextensionsctl list` fails with `OSSystemExtensionErrorDomain error 1`. The available Apple team entitlements and a signed dext match therefore remain unverified.

## Decision

Use the same AudioServerPlugIn plus user-space USB architecture that already works in the prior driver, but implement it cleanly in `numark-ns6/`. The output callback writes only to a bounded ring; the transport worker owns `0x02`; the feedback reader owns `0x81`; one device-session object arbitrates interface/configuration lifetime. Do not add a separate privileged helper unless direct implementation testing demonstrates the plug-in process cannot own the needed interface.

The unprivileged descriptor probe failed, while the elevated read-only probe succeeded. The active driver prevents a second client from opening interface 0, which is the expected ownership boundary. Keep the installed driver running during development; remove it only immediately before the first test installation of the new package.

## Implementation boundary

Proceed with Audio OUT alone. Resolve USB access in the new device-session module, then publish the output device through AudioServerPlugIn. Before the first installation test, remove the old driver versions and their agents/extensions as requested; until then, leave the installed system untouched.

## Sources

- [Apple AudioDriverKit overview](https://developer.apple.com/documentation/audiodriverkit)
- [Apple DriverKit entitlement requirements](https://developer.apple.com/documentation/driverkit/requesting-entitlements-for-driverkit-development)
- [Apple IOUSBHost overview](https://developer.apple.com/documentation/iousbhost)
- [Apple AudioServerPlugIn StartIO contract](https://developer.apple.com/documentation/coreaudio/audioserverplugindriverinterface/startio)
