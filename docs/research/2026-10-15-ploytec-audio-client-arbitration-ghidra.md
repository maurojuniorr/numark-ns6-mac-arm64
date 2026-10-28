# Ploytec audio-client arbitration — Ghidra pass 25 (2026-10-15)

Read-only Ghidra 12.1.4 analysis of the original x86_64 `NumarkNS6Audio` kext, checked against the installed arm64 driver's unified logs and current source. No driver code or runtime settings were changed.

## What the Ploytec output path does

- `PGDevice::rtsProcessIsocOut_Std` (`0x173ba`) asks `rtsGetMinAvailableFrames` (`0x17346`) for the minimum available frames across attached clients before consuming the next output group. When the USB group is near its scheduled deadline and that minimum is too small, it leaves the group pending for a later completion rather than advancing the client rings early.
- The output path uses one common presentation-frame cursor. `ClientData__rtsIsocOutputSamples` (`0x7ef2`) reads each client's `AudioSM` ring at that cursor; `DriverData::rtsIsocOutput` (`0x7f78`) accumulates attached-client samples in a working buffer, saturates the sum, applies the selected output mixer, and encodes the USB payload.
- This is a per-client ring plus a separate USB consumer/mixer. It is not a shared FIFO to which each client's callback is appended as the next audio in time.
- The normal isochronous completion path `PGDevice::rtsIsoWriteComplete` (`0x18e86`) requests the next transfer while on time. If it has missed the scheduled USB frame, it stops streaming and reports `STC_STREAM_BROKEN`; the original driver is not immune to a missed deadline.

## Keep-alive path: important scope limit

The kext also has `rtsIsocAudioRunKeepAlive` (`0x17290`) and a separate `IsocData` queue at device offset `+0xa38`. It initially submits three silent output requests. Its completion requeues the next keep-alive transfer; if late, it advances the scheduled frame and still requeues instead of stopping the stream.

However, `resumeStreaming` (`0x16ba4`) selects this path only in its alternate bulk-audio branch. The NS6 descriptor set used by the normal path has isochronous OUT `0x02` and feedback/status IN `0x81`, so this separate bulk-mode keep-alive is not evidence that the original NS6 playback path keeps an extra idle stream open. Do not copy it as the fix for concurrent Mixxx/Opera output.

## Comparison with our driver and today's event

- The current HAL advertises `WriteMix` and `ReadInput`, not `MixOutput`. Each `WriteMix` callback appends its block to the single shared `NS6Transport` ring. The callback's sample time is not used to align or sum data from separate clients.
- The 0.2.195 experiment did receive per-client `MixOutput` callbacks; it did not receive one unified final block. Appending those callbacks sequentially to the same FIFO was therefore not a mixer and was correctly rolled back. The installed HAL bundle now reports 0.2.196.
- At 16:14:38.761–39.352, the current logs show Mixxx's output flow going inactive and restarting about 591 ms later. Its maximum `WriteMix` callback gap reached 646.149 ms, while callback execution stayed below 26 μs. The 16:14:44.824 USB health snapshot reported only 2 underrun events / 6 missing frames, 3,675 queued frames (~83 ms), and zero USB transfer, packet, or short-packet errors.
- Therefore the callback/flow pause is real, but this snapshot does not show a 646 ms empty USB queue. It also does not establish the cause of the reported full Mixxx UI freeze; that needs Mixxx hang diagnostics if the UI itself stopped responding.

## Strategy indicated by the evidence

The useful Ploytec pattern to evaluate is separate per-client staging with sample-time alignment, a single audio-thread-safe mixer/consumer, and an explicit policy for clients that stop or temporarily have no block. Inactive clients must not hold the output deadline open; their contribution should be silence. Active clients must be mixed at the same presentation time, never concatenated. Keep the USB transfer queue and the CoreAudio client buffers as separate quantities.

The first candidate regression tests use two clients writing overlapping sample-time ranges, a late client block, a stopped client, and concurrent writes. They verify that simultaneous samples are summed once, a stopped client no longer blocks the remaining client, and gaps remain visible instead of being silently concatenated. A larger common FIFO alone does not implement this behavior.

The Ploytec kernel's `AudioSM` results are static x86_64 disassembly. They describe the legacy implementation, not proof of identical CoreAudio scheduling on this Apple Silicon system.

## Working-tree implementation candidate

The HAL now stages up to 16 CoreAudio clients in separate 16,384-frame rings in the existing shared-memory object. `WriteMix` passes the client's ID and `mOutputTime.mSampleTime`; the USB-side reader sums only samples with matching presentation times and saturates the four-channel 24-bit result. A client that has not produced its first block is ignored until it joins at its timestamp; a stopped client is removed from arbitration, and its ring slot can be reused. The shared-memory protocol version is now 4 so mismatched HAL/service binaries reject the incompatible layout.

Focused tests cover overlapping clients, late joins, stopped clients, restart/slot reuse, timestamp gaps, concurrent client writes, and shared-memory IPC. These are software tests only. The candidate has not been installed or exercised with Mixxx and Opera together; actual CoreAudio callback timing and the policy for a client's temporary gap still require hardware validation. Sixteen staging rings add roughly 5 MiB to the shared transport object.
