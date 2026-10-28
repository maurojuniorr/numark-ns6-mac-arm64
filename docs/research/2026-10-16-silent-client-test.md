# Active silent CoreAudio client diagnostic

0.2.135 remains installed. No package was installed for this diagnostic.

At 19:14:17 a temporary client opened the NS6 output and supplied zero PCM. It is bounded to 180 seconds and stops/destroys its IOProc on completion. Its purpose is to separate the HAL idle lifecycle from the presence of audible samples. The original Ploytec capture also contains all-zero PCM in EP 0x02 isochronous packets, so nonzero keepalive samples are not established as a requirement.

Before the test, the helper recorded repeated EP 0x83 NotResponding and pipe clears up through 19:14:05 (clear epoch260). The first query after opening the silent client shows no further MIDI errors, pipe clears or thresholded USB callback gaps. Final observation and user LED report pending.

Do not treat the temporary client as a production fix; it keeps CoreAudio rendering and consumes resources even when user applications are closed.

## Result

- 19:14:17.064: silent client opened; three minutes, 15,576 callbacks.
- User opened/closed apps and reported no LED blink while this client was active.
- No MIDI errors, pipe clears or thresholded USB callback gaps were found during that interval.
- 19:17:17.916: bounded diagnostic client exited and HAL entered idle.
- Another app opened at19:17:20.989 and finally closed at19:17:30.107.
- 19:17:30.503: first MIDI NotResponding, 396ms after idle. The read was82.688s old, which contradicts a fixed9–10s timeout. User reported a blink at this later closure.

This supports the last-client lifecycle/scheduling hypothesis, not a requirement for audible/nonzero audio. It does not isolate every internal scheduling mechanism from the HAL paused-output branch.

## 0.2.137 experiment

The existing MIDI bridge checks the NS6 by exact device UID and `DeviceIsRunningSomewhere`, waits for the first external audio session, and retains a zero-output IOProc until device removal or bridge exit. Checks run every250ms before retention and every1s afterwards. No buffer size/default-device preference is changed. The MIDI/USB code matches0.2.135; the unvalidated zero-timeout API experiment0.2.136 is not included.

The same production retention function was tested independently on hardware: it stayed inactive initially, acquired the device after a separate three-second audio client started, stayed active after that client closed, and released its IOProc cleanly at test exit. Parser and recovery unit tests passed; package builds/signatures passed. Full installed bridge, unplug/reconnect, CoreAudio restart and longer CPU/energy impact remain to validate.

Tradeoff: audio rendering remains active while the controller is retained, increasing idle work. The bridge may appear as an active client in Status; this is intentional diagnostic visibility. Very short initial audio sessions below the polling period may not be caught. This remains a test build, not a stable release.

## 0.2.138 reconnect experiment

The first 0.2.137 hardware observation showed that power-cycling the NS6 invalidates the held CoreAudio device ID. The bridge logged `AudioDeviceStop: no device with given ID`, released the old IOProc, and did not recreate it until a real app opened audio. The LED being lit immediately after hardware power-on therefore did not prove that the silent audio IOProc was active.

0.2.138 removes the requirement for `DeviceIsRunningSomewhere` before creating the silent IOProc. The bridge now starts it whenever the NS6 is present and alive, including after UID translation returns a newly enumerated device following a power cycle. The idle-rendering cost is now continuous while the NS6 is connected; hardware validation after installing 0.2.138 is pending.
