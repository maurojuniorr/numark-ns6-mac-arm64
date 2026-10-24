# Numark NS6 Audio IN module

**Goal:** Implement a testable stereo capture module for the NS6 Master/Record return, without coupling it to the playback ring or publishing an unverified CoreAudio input stream.

**Evidence:** USB capture evidence identifies interface 1, bulk IN `0x86`, 10,240-byte blocks, and a 64-byte stride carrying two 24-bit bit lanes. Input setup reads vendor request `0x49` and selects mode `0x32`; the known startup also selects 44.1 kHz clock sources 134 and 2. The NS6 returns the Master/Record mix: software playback routed through the mixer plus analog sources routed to Master.

## Implemented

- `AudioInputDecoder`: bounded conversion of each 64-byte source stride to stereo Float32.
- `AudioInputEngine`: independent capture ring; malformed, overflow, and starvation counters; zero-fill on missing frames.
- `AudioInputReader`: four independent 10,240-byte bulk reads on `0x86`, completion ordering, isolated stop/drain, and transport counters.
- `NS6DeviceSession`: input mode/clock setup and shared event-source lifetime with feedback; restores the original input mode at close.
- `midi/ControllerActivation`: independent, optional host-identify/jog-enable/wake packet sequence for diagnostic use.
- `ns6_audio_input_probe`: direct hardware capture diagnostics; optional silent OUT to test the NS6 mix bus.

## Validation still required

- The decoder and engine tests pass under ASan/UBSan.
- The latest privileged probe ran for two seconds with silent OUT: 501/509 OUT transfers completed, zero OUT failures, zero input blocks, and zero input transport errors. This is not a passing capture test; the endpoint remained pending.
- An optional controller-activation attempt timed out on its first MIDI OUT write (`kIOUSBTransactionTimeout`, `0xe0004051`); it did not reach the jog-enable or wake messages. The physical awake/power state and the precise reason the MIDI endpoint does not acknowledge remain unknown.
- Before publishing an input stream, determine why this hardware state does not produce `0x86` blocks. Compare a powered/awake NS6 against standby; then test non-silent Master-routed playback and an analog source. Keep any controller activation mechanism as its own module.
- After the transport produces blocks, verify channel order, level, malformed/truncated blocks, sustained capture, and simultaneous capture + playback + MIDI. Only then expose a separate two-channel CoreAudio input stream using the same device-wide client lifecycle and a distinct ring/worker.

**Explicit boundary:** this work does not create an AudioServerPlugIn, install a driver, integrate activation into capture demand, or claim independent mic/line input channels.
