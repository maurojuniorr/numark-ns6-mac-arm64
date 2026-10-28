# NS6 audio-capture transport evidence

## Summary

The original Ploytec kext uses the NS6's bulk-IN endpoint `0x86` for audio capture. It is not a conventional PCM endpoint: the kext reconstructs two 24-bit sample streams from the low bit of byte lanes in each bulk block, then places them in 32-bit CoreAudio samples. A synchronized QuickTime recording from the explicitly selected NS6 input matches the reconstructed signal envelope (correlation 0.996). This is now direct hardware-capture evidence for the input transport.

## Hopper observations

- `PGConfigurationDescriptor::findAudioStreamingInInterface` and `PGInterfaceDescriptor::isAudioStreamingIn` are generic Ploytec descriptor routines. The NS6 does not advertise a standard class-compliant PCM capture endpoint, so those routines alone did not identify the NS6 input path.
- For the NS6, `PGDevice::bulkAudioRun` configures the input format, chooses a DMA decoder, allocates four bulk-IN IRPs, and starts reads on `m_pcInPipe`. `bulkAudioInCompletion` passes completed blocks to `bulkInpEvent`; `rtsProcessBulkIn` invokes the selected decoder before publishing frames to the shared audio ring.
- `PGDevice::chooseDMADecoder` selects `_dmaDecode16To02At32Bit` when converting the 16-channel DMA source mode to two CoreAudio channels. Its C implementation takes the low bit from 24 bytes at offsets 0–23 and 24 bytes at offsets 32–55, assembles two signed 24-bit values, shifts them into 32-bit samples, and advances by a 64-byte source stride. The `16` in the decoder name is the DMA source-channel mode, not a 16-bit sample depth. This explains why the captured bytes look like only `0x00`/`0x01` rather than ordinary PCM.

## NS6 USB capture observations

Artifact: `/Users/maurojunior/Downloads/ns6_full_10min.pcapng`, USB Darwin capture from macOS 10.14.6 on XHC20. TShark identified NS6 device address 13. Full-capture endpoint/type counts:

| Endpoint | Transfer | Direction | Captured transfers |
| --- | --- | --- | ---: |
| `0x02` | isochronous | host → NS6 | 300,056 |
| `0x81` | isochronous | NS6 → host | 300,056 |
| `0x86` | bulk | NS6 → host | 330,826 |
| `0x83` | bulk | NS6 → host | 4,270 |
| `0x04` | bulk | host → NS6 | 19,472 |

Endpoint `0x81` carries feedback/status traffic, not capture samples. Endpoint `0x83`/`0x04` carry MIDI. Endpoint `0x02` is playback. The earlier 10-minute capture did not correlate `0x86` to a recording, so the project protocol notes had labeled it waveform data. That interpretation is superseded by the synchronized input test below; the `0x00`/`0x01` byte values are expected by the Ploytec decoder.

### Synchronized NS6 input capture (2026-10-14)

Artifacts: `/Users/maurojunior/Downloads/mic2.pcapng` and `/Users/maurojunior/Downloads/mic2.m4a`. QuickTime's input source was explicitly set to **Numark NS6**. The capture shows bulk-IN endpoint `0x86` beginning when QuickTime opens the input. It contains 10,240-byte completions; all captured payload bytes are `0x00` or `0x01`.

Applying the Ploytec two-channel decoder layout to those payloads reconstructs 1,721,760 frames over 39.051 seconds (about 44.09 kHz from first/last completion timestamps; transfer-queue boundaries make this an estimate, not a clock measurement). The reconstructed signal's 10 ms RMS envelope matches the QuickTime recording at 0.996 correlation, with the recording beginning about 7.61 seconds after the first captured input block. The correlation remains 0.991 when low-level bins are excluded, so shared silence does not explain the match. Together with the kext's decoder and bulk-IRP path, this confirms that `0x86` carries the NS6's audio input. The unusual binary-looking payload is a bit-lane encoding that the Ploytec decoder expands; it is not ordinary 16/24-bit PCM on the wire.

The same trace captures the driver's mode negotiation immediately before streaming: vendor control IN request `bRequest=0x49`, `wLength=1` returns `0x12`; the driver then sends vendor control OUT `bRequest=0x49`, `wValue=0x0032`, with no data stage. This matches `PGDevice::setAJDMAInputChannels`, which reads one mode byte, updates its flags, and writes the selected mode back. Bulk-IN reads on `0x86` begin immediately after this setup.

## Descriptors from the connected NS6

On 2026-10-06, the read-only `tools/ns6-probe` queried the physical NS6 (`15e4:0079`). Its single 80-byte configuration has only these alternate settings:

| Interface | Alternate | Endpoint | Transfer | Max packet |
| --- | ---: | --- | --- | ---: |
| 0 | 0 | none | — | — |
| 0 | 1 | `0x02` | isochronous OUT | 156 |
| 0 | 1 | `0x83` | bulk IN | 512 |
| 0 | 1 | `0x04` | bulk OUT | 512 |
| 1 | 0 | none | — | — |
| 1 | 1 | `0x81` | isochronous IN | 64 |
| 1 | 1 | `0x86` | bulk IN | 512 |

No alternate setting exposes another input endpoint. The input therefore shares an existing endpoint: the Ploytec kext uses bulk-IN `0x86`; `0x81` remains the feedback/status endpoint.

## Experimental implementation status (2026-10-14)

The arm64 driver now has a first experimental capture path in the working tree:

- It reads the current input-mode byte with vendor request `0x49`, then selects the observed two-channel mode (`0x32`) before streaming.
- It decodes the two 24-bit bit-lanes from each 64-byte source stride and places stereo float frames in an SPSC capture ring.
- It exposes a separate two-channel CoreAudio input stream, ID `4`, while preserving the existing four-channel output stream, ID `3`.
- CoreAudio `ReadInput` callbacks consume decoded frames; if the capture ring is empty, the remaining requested frames are zero-filled.
- EP `0x86` polling is demand-gated while a MIDI session is active. This avoids continuously claiming the endpoint in MIDI-only use, based on prior observations that continuous reads could interrupt MIDI. If there is no MIDI session, the existing endpoint drain behavior remains active.

The first hardware test succeeded on 2026-10-14. QuickTime recorded `/Users/maurojunior/Desktop/micns6.m4a` through the NS6 input on the first attempt. The file is stereo AAC at 44.1 kHz, 12.72 seconds long; decoded audio has a peak of about −7.66 dBFS and RMS of about −30.9 dBFS. In 50 ms windows, none were digital silence (minimum RMS `9.67e-5`). This confirms that the experimental HAL input stream reaches an application and carries nonzero audio.

The user then confirmed that the recording contains both the microphone and computer playback. This matches the NS6's documented recording route: the VirtualDJ NS6 guide says the interface's stereo record input captures the Master output, including the microphone and other analogue sources; it also notes that the mic/line inputs are not exposed as independent software inputs. The current driver has distinct HAL input/output streams and does not mix them in software; the combined signal is therefore the NS6's record/mix bus. We should describe this stream as a stereo record mix, not an isolated microphone input. See [VirtualDJ's NS6 advanced audio setup](https://www.virtualdj.com/manuals/hardware/numark/ns6/advanced.html).

In the first combined input/Mixxx playback/MIDI test, the user heard a few pops while an external LINE connection and a microphone were both connected. MIDI controls remained responsive. Removing the LINE connection made playback clean. This points to the LINE source or its routing/level as the trigger in that test, rather than a demonstrated USB/MIDI failure; the exact electrical or routing cause is not established. The driver was installed as 0.2.156 during this observation.

The driver and package compile, the decoder/ring tests pass, and the package is signed at the bundle level. Remaining hardware checks are channel correctness, sustained callback continuity, and simultaneous input with MIDI and playback. Independent MIC/LINE capture is not expected from this two-channel record-mix path. In particular, simultaneous input plus MIDI remains unverified because earlier tests found that continuous EP `0x86` reads could interrupt MIDI. The package is experimental, unsigned as a macOS installer package, and must not be treated as a stable release.

## Limits

- The 2026-10-14 input capture was one selected-source recording; it does not yet separate MIC and LINE routing or establish whether both can be captured simultaneously.
- The measured rate is estimated from USB completion timestamps and queued transfer sizes, not from a hardware clock counter.
- The QuickTime audio is lossy AAC; envelope correlation confirms matching activity, while bit-exact audio comparison is not expected.

## Ghidra pass 2: Ploytec isochronous feedback and packet sizing (2026-10-12)

Artifact analyzed: `/Users/maurojunior/Downloads/NumarkNS6Audio.kext/Contents/MacOS/NumarkNS6Audio`, using Ghidra 12.1.4 headless analysis. The project and decompiler output are currently in `/private/tmp/ns6-ghidra-pass2/`; these are temporary analysis artifacts, not checked into this repository. Findings below are from decompiled control flow and should be checked against the original disassembly before treating inferred field meanings as exact.

### Variable packet lengths are part of the driver's normal isochronous path

- `PGDevice::resumeStreaming` initializes the frame-pattern observer, then selects the isochronous audio path when the configured pipes are isochronous. The path starts the audio stream and its feedback reader; when bulk audio is paired with isochronous audio, it also starts a keep-alive stream.
- `PGDevice::initFramePattern` initializes `PGFramePatternObserver` from the active sample rate, channel counts, bit depths/bytes per sample, and device identity.
- The Ghidra decompilation of `PGFramePatternObserver::init` exposes the nominal 44.1 kHz high-speed pattern directly. In its high-speed mode it seeds a 128-entry history with 5-frame packets, adds a frame on odd entries, and adds one at each 80-entry boundary. Across the first 80 service intervals this yields 41 packets of 6 frames and 39 packets of 5 frames: 441 frames per 80 intervals, or exactly 44,100 frames/s at 8,000 intervals/s. For four 24-bit channels, those become 72-byte and 60-byte packets. This makes the packet-size mechanism explicit in the kext, not merely an inference from `initWrite_Std`.
- The feedback callback (`PGDevice::isocReadCompleteFeedback`) dispatches to one of two feedback decoders based on a pipe field. The normal decoder accepts frame counts within a learned min/max interval; the full-frame decoder accepts reports within ±2 of its expected frame count. Valid feedback updates a circular pattern history. Invalid feedback unlocks the observer and reuses its prior predicted pattern until the pattern locks again.
- `PGIsocRB::initWrite_Std` reads the next frame count from that circular pattern for every isochronous packet, sets each packet's requested byte count to `bytesPerFrame × framesForThisPacket`, and advances the transfer offset. This is direct evidence that Ploytec intentionally emits packets of different lengths to track fractional frames per USB service interval. It is a strong architectural match for Gregory's whole-frame packet proposal; it does not prove the exact NS6 packet pattern without matching the active endpoint mode and comparing a USB trace.
- The output completion observer (`PGFramePatternObserver::isocReadComplete`) derives frames from each completed packet's actual byte count. If the result is outside the expected range, it marks synchronization lost, substitutes a predicted packet size, and zero-fills the corresponding payload tail before continuing. This is explicit recovery behavior around packet-size deviations.

### Feedback scheduling and failure recovery

- `PGDevice::requestIOFeedback` obtains the next feedback request from an isochronous ring and submits an IN transfer on the feedback pipe. On completion, `PGDevice::initNextTransactionFeedback` reads the USB frame counter and schedules the next feedback request; if its target is already behind the current frame, it advances the target by four frame units. The units are not yet proven to be milliseconds, so retain the code's frame-counter terminology.
- Transfer completions with a non-success status increment an error counter. A valid decoded feedback update clears the counter. Once it exceeds 40, the callback logs `mIsocInFeedback.mErrorCounter > 40`, stops streaming, and sends a streaming-state notification to clients. This gives the original driver an explicit recovery boundary for repeated feedback-transfer failures.
- `PGDevice::handleStreamBroken` has a separate restart path: after a short delay it reads current configuration, reconfigures clients, calls `startStreaming`, and notifies clients. The code supports automatic stream recovery, although the exact event that enters this path still needs call-site tracing.

### What this changes for our implementation

1. Treating EP `0x81` as feedback is consistent with both the captured 44/45-byte values and the Ploytec feedback-reader architecture. It is no longer reasonable to model this endpoint as an unused constant response.
2. The Ploytec design supports per-packet whole-frame sizing driven by feedback/history, rather than correcting only a long-term scalar sample-rate estimate. Our fixed-size 66-byte path therefore differs from the original driver's apparent packet-shaping strategy.
3. Before porting more behavior, compare one Ploytec USB trace against the exact expected NS6 interval pattern and identify which feedback-decoder branch is selected. The decompilation alone does not establish whether the kext sends 66-byte packets, nor whether it accepts the same endpoint/frame constraints as our current macOS userspace/CoreAudio implementation.
4. The generic `setAJDMAInputChannels` and `bulkAudioRun` routines are not evidence that this kext exposes the NS6's analog inputs. Their device-specific branches are keyed to another VID/PID (`0x0a4a` and product `0xd064`), not the NS6 (`0x15e4:0x0079`); input support remains unproven.

## Ghidra pass 3: NS6 feedback-pipe selection and decoder mode (2026-10-12)

This pass traced the device-specific predicate, pipe assignment, and the full-frame feedback decoder. It closes the question of whether the 44/45 reports are merely coincidental values on an unrelated endpoint.

- `PGDevice::isPloytecBulkInIsocOutDevice` explicitly recognizes Numark VID `0x15e4`, PID `0x0079` (the connected NS6), when the kext's USB-2/high-speed flag is set. In `PGKernelDeviceNUMARKNS6::configureDevice`, that branch clears the ordinary isochronous audio-input pipe and assigns `m_pcInPipeFeedback` from `getIsocInPipe` on the selected streaming-input interface. The NS6 descriptor probe and pcap identify that physical isochronous IN endpoint as `0x81`.
- The feedback reader is not a separate generic input stream: `requestIOFeedback` submits reads on `m_pcInPipeFeedback`, and `isocReadFeedbackCompleteStatic` copies the first byte from each completed ISO slot into the feedback array before calling `PGDevice::isocReadCompleteFeedback`.
- For a feedback pipe whose descriptor field at offset `+0x16` equals `4`, the callback selects `PGFramePatternObserver::isocFeedbackReadCompleteFullFrame`. In the 44.1 kHz high-speed initialization, the observer expects roughly 44 frames per service interval and accepts 42–46. A received value selects the precomputed eight-packet pattern for that frame count; the five candidate patterns are initialized for totals 42, 43, 44, 45, and 46. The normal 44-frame candidate is the 5/6-frame schedule described above.
- Out-of-range bytes such as `0xAA` do not get interpreted as valid frame counts: they invalidate the current lock and substitute the observer's predicted pattern while it reacquires. This matches the pcap's triplet-plus-placeholder layout: the callback sees the first byte of each three-byte ISO slot, so the `0x2C`/`0x2D` slots are usable reports and `0xAA` slots fall through prediction. The `0xAA` slots are not evidence that the feedback pipe is dead.
- `PGIsocRB::initWrite_Std` consumes the selected per-packet frame schedule and sets each OUT descriptor length to `bytesPerFrame × frames`. Together with the pcap's 60/72-byte packets, this now establishes the intended feedback-to-variable-packet architecture end to end at the code-path level.

The remaining uncertainty is narrower: the pcap was not captured with a verified build identity for the exact Ploytec kext, and its feedback slots are mostly placeholders. It therefore does not prove the precise timing/phase relationship between each `0x2C`/`0x2D` report and the corresponding 60/72-byte OUT packets, nor the exact USB-speed flag value used in that session. A new capture is only needed to settle that interval-by-interval correlation or identify the loaded kext build; it is not needed to establish that EP `0x81` is the NS6 feedback path and that the Ploytec NS6 code uses feedback to select whole-frame OUT packet sizes.

## Ghidra pass 4: playback queue scheduling and late-transfer recovery (2026-10-12)

- `PGDevice::rtsIsocAudioRun` initializes the input, feedback, and output isochronous rings with a start-frame value of `getCurrentFrame() + 0x32`, then primes three input reads, three output writes, and two feedback reads. The observed `0x32` lead is a USB frame-counter offset in the code; do not translate it to milliseconds without confirming the host controller's frame-counter units.
- Each `rtsRequestOut` takes the next free output ring buffer, builds its per-packet payload lengths from `PGFramePatternObserver`, and submits a low-latency isochronous write. On normal completion, `rtsIsoWriteComplete` checks the current USB frame against the ring's scheduled target and queues the next output transfer. This is a rolling queue, not a single write call per CoreAudio callback.
- If completion is already at or beyond that scheduled target, the kext logs the transfer as late, moves its target to current frame + 5, stops streaming, marks the stream broken, and notifies clients. It does not silently continue indefinitely with a stale schedule. The keep-alive writer has a separate path that sends one-byte isochronous packets and resubmits itself, but that is distinct from the normal NS6 output path.
- These functions show a useful diagnostic distinction for our driver: a periodic audio defect with USB transfer errors differs from a host scheduling miss that makes the queued isochronous transfer late. The kext has an explicit late-transfer boundary, while our current health counters primarily report transfer/packet errors and queue depth. This suggests instrumenting scheduled-vs-actual completion timing if we want to distinguish those causes; it does not by itself prove that late transfers cause the glitches reported on this Mac.

## Ghidra pass 5: client audio to USB payload (2026-10-12)

- `PGDevice::rtsProcessIsocOut_Std` walks the queued output descriptors, derives the audio-frame count from each descriptor's byte count and bytes-per-frame, and passes the output region to `DriverData::rtsIsocOutput`. It tracks one monotonic output-frame cursor across packets and checks the earliest available client frames before processing each group. The decompiler shows a deadline-sensitive defer path when the current group is near its USB schedule and the client ring does not yet have enough frames; exact field semantics remain inferred because the client ring implementation is called through `AudioSM::read`.
- `DriverData::rtsIsocOutput` converts the outgoing payload size to an internal sample count, reads each attached client through `ClientData__rtsIsocOutputSamples`, accumulates clients in a 32-bit working buffer, saturates the mix, applies the selected output mixer, and encodes the result into the USB payload. When the active-output setting is disabled, it zero-fills the payload. `ClientData__rtsIsocOutputSamples` delegates the actual ring read to `AudioSM::read` and uses a client-generation check to detach stale client state safely.
- The generic NS6 24-bit path in `PGDevice::chooseISOOutEncoder` falls back to `pcmTo24LSB`; channel-swap and other special converters are selected only for explicit device/configuration cases. The descriptor evidence identifies the NS6's four-channel output as 24-bit, matching the 12-byte frame size used to interpret the 60/72-byte packets.
- The `AudioSM` implementation is present in the same binary. Both `AudioUserClientNUMARKNS6` and `ASClientNUMARKNS6` allocate a `0x83ff0`-byte shared audio region and expose it as memory type `5` through their user-client memory APIs. `SharedMemory::map` maps that region into the client task. The binary therefore establishes a shared-memory audio interface; it does not include the user-space HAL writer that fills the client side of the ring.
- `AudioUserClientNUMARKNS6::initIOData` reads the active `ChannelSettings` fields. It initializes `AudioSM` using the configured output channel count when nonzero, otherwise the input channel count, plus the active frequency and an extra-per-frame setting. The NS6 property publisher names these fields `Frequency`, `InChannels`, `InResolution`, `OutChannels`, `OutResolution`, and `Configuration`. The connected NS6 exposes four output channels; with four 32-bit ring channels and no extra per-frame data, the formula gives a 32,768-frame capacity (about 743 ms at 44.1 kHz). Treat this as the capacity for that configuration, not a measured application queue depth.
- `AudioSM::init` reserves the first `0x80000` bytes for sample storage and selects the largest power-of-two frame capacity up to `((0x80000 / (channels × 4 + extraBytesPerFrame)) & 0xfff00)`. `AudioSM::read` wraps at that capacity, copies requested frames, advances the reader cursor, and interpolates timestamps. It has no availability check of its own.
- `rtsGetMinAvailableFrames` separately checks attached clients; `rtsProcessIsocOut_Std` uses that result to defer a near-deadline output group when the rings do not contain enough frames. The kernel xrefs show `ClientData__rtsIsocOutputSamples` calling `AudioSM::read`; they also show `rtsProcessIsocOut_Std` calling `AudioSM::writeFrames` with a null sample pointer in one path, which advances its frame/timestamp bookkeeping without copying new samples. So `writeFrames` should not be characterized as the application's producer based on this binary alone.

This pass identifies the kernel side of the shared-memory boundary, the active channel-setting fields, and the ring-capacity calculation. It still does **not** reveal the user-space writer's scheduling policy, normal ring occupancy, or the actual buffer size requested by Mixxx/CoreAudio. Nor does the large ring capacity establish playback latency; that requires runtime occupancy or client-side HAL evidence. These limits prevent a direct one-to-one port of the Ploytec ring size into our CoreAudio driver.

## Ghidra pass 6: original CoreAudio HAL scheduling (2026-10-12)

The original HAL component was present on this Mac at `/Library/Audio/Plug-Ins/HAL/NumarkNS6AudioHAL.plugin`, bundle ID `com.numark.ns6.hal`, version 3.3.11. Its Mach-O contains x86_64 and i386 slices, with C++ symbols and source-file references. The universal binary SHA-256 is `f7f490de5713c6610c2a51698a0fc774b07c13db0e9f3cf3f73b02ef5c84d5d9`. I analyzed the x86_64 slice in Ghidra 12.1.4. This is the legacy Ploytec HAL, not our currently installed `NumarkNS6M1.driver`.

- `PGAsioDriver::onDeviceSetProperty` handles `kAudioDevicePropertyBufferFrameSize` by calling `PGOSXDevice::setFrameSize`. That method clamps the requested frame count to the supported range, updates the `frameSize` property, rebuilds its local input/output `AudioIOBuffer`s, and recomputes latency properties. While streaming, it records a pending frame-size change; `PGOSXDevice::onUSBProc` applies that change in its processing loop. This confirms the legacy HAL has a real per-device CoreAudio block-size path. That block size is separate from the USB packet frame pattern handled by the kernel driver.
- `PGOSXDevice::threadRun` runs a worker thread. Once the device is ready, it attaches using the selected frame size and repeatedly calls `onUSBProc`, sleeping for the nanoseconds returned by its timing logic. This user-space processing thread is distinct from the kernel's rolling isochronous USB queue.
- `PGOSXDevice::onUSBProc` checks the shared input/output `AudioSM` regions and channel counts, then measures available frames in the input ring against the output ring's current frame position. If fewer than one selected application block are available, it asks `TimestampFilter::calcSleepNanos` when to wake. When enough frames exist, it processes at most two blocks per loop: reads the input ring, calls the registered CoreAudio IOProcs, then publishes the rendered output block to the shared output ring with `AudioSM::writeFrames`. If the IOProc is not active, it writes silence to the output block. This identifies the user-space producer that the kext-only pass could not show.
- `TimestampFilter` derives its timing from the active sample rate, host timebase, frame positions, and CoreAudio host timestamps. It prepares block timestamps, updates a filtered host-time estimate, and returns a sleep interval based on the difference between available frames and the selected frame size. Its logs distinguish late processing/overload cases; this is a pacing mechanism, not USB feedback or a replacement for the kext's packet-size observer.
- The NS6-specific branch in `PGOSXDevice::updateLatencies` adds the selected `frameSize` to both its output write-ahead base and one of the two latency values returned for the device. Before that NS6 adjustment, the write-ahead base includes two frame blocks plus a sample-rate term; the NS6 branch adds a third frame block. The other latency value starts at 150 frames unless another device-specific rule overrides it. This is direct evidence that the legacy HAL deliberately ties its output scheduling and reported latency to the application-selected buffer. It does not justify copying the numerical formula into our modern driver without matching the property's semantics and measuring the active path.

This closes the static path from a CoreAudio IOProc callback through the HAL's local buffers and shared-memory output ring to the kernel USB encoder. It also explains why a large shared ring capacity is not the application's selected buffer: the HAL uses the selected frame size as its work block, while the ring is a larger transport buffer paced by timestamps. The remaining unknowns are runtime ring occupancy and whether this exact 3.3.11 HAL was loaded during the historical USB capture; bundle presence and binary analysis alone do not prove either.

## Ghidra pass 7: output readiness is gated by the slowest attached client (2026-10-13)

The focused decompilation of `rtsGetMinAvailableFrames` (`0x17346`), `rtsProcessIsocOut_Std` (`0x173ba`), `ClientData__rtsIsocOutputSamples` (`0x7ef2`), and `AudioSM::read` (`0x2a090`) clarifies how the legacy driver arbitrates several audio clients before filling a USB output group:

- `rtsGetMinAvailableFrames` scans eight client slots. For each slot with both its attachment and shared-memory references set, it evaluates that client's frame/timestamp position relative to the common output-frame cursor, keeps the minimum result, and counts eligible clients. The output path therefore uses the least-ready client as its readiness bound; it does not average client queue depths.
- `rtsProcessIsocOut_Std` compares that minimum against the frame count of the next packet/group when the USB schedule is near its deadline. If the minimum is too small, it leaves the group pending for a later completion instead of consuming the client rings immediately. Outside that near-deadline condition, the same check does not defer the group. The decompiler's local names and timing units are not reliable enough to assign a wall-clock duration to this threshold.
- When output proceeds, the kernel passes one monotonic frame cursor to `DriverData::rtsIsocOutput`; each attached client's wrapper seeds its `AudioSM::read` cursor from that same position. `AudioSM::read` wraps the cursor at ring capacity and advances it by the requested frame count, but does not itself test whether that many frames are available. Readiness protection therefore resides in the caller's client scan and deadline gate, not inside the ring read routine.

This adds an important architectural detail to the earlier shared-ring findings: client readiness is coordinated against a shared presentation cursor, and a lagging attached client can hold back a near-deadline USB output group. It does **not** establish that this path caused the glitches observed with our driver, nor does it justify copying the kext's client policy into our single-`WriteMix` transport. The current driver has no equivalent collection of independent per-client `AudioSM` rings; diagnosis should continue to rely on its own callback, queue, USB-completion, and underrun counters.

## Ghidra pass 8: feedback-pattern reacquisition (2026-10-13)

This pass follows `PGFramePatternObserver::isocFeedbackReadCompleteFullFrame` (`0x1a49c`) and `PGDevice::isocReadCompleteFeedback` (`0x13fa6`):

- In full-frame mode, an in-range feedback value selects one of the precomputed eight-packet patterns. An out-of-range value clears the observer's locked flag, resets its reacquisition reference to the current history cursor, and copies the nominal predicted eight-packet pattern into that history slot. The 128-entry high-speed history advances by eight entries per full-frame value. Therefore the lock can only return after the cursor makes a complete history pass without another invalid value resetting the reference; the exact elapsed time depends on the number of values in each USB callback and on how often invalid values arrive.

The observer treats a bad feedback value as loss of confidence in timing, not immediate loss of audio: it keeps packet shaping alive with a predicted pattern while it reacquires its history lock. It also shows that the feedback decoder's nonzero return is a lock-reacquisition signal in this mode, not a per-packet “feedback is valid” flag.

## Ghidra pass 9: sustained feedback errors stop streaming and trigger recovery (2026-10-13)

The feedback callback increments its error counter for ISO descriptors whose completion status is not success. `isocReadCompleteFeedback` clears that counter only when the observer decoder returns nonzero. Once the counter exceeds 40, the callback calls `stopStreaming(true)` and sends streaming notification code `2`; it returns without submitting the next feedback read. This escalates sustained transfer trouble instead of endlessly retrying a broken feedback request.

`stopStreaming(true)` marks streaming inactive, cancels the input/output/keep-alive rings, aborts the relevant USB pipes and asynchronous port, waits 100 ms, and then runs the stopped callback. Separately, `handleStreamBroken` (`0x16924`) has a two-stage recovery path: its second stage waits 100 ms, prepares the current configuration, and—when the driver's streaming-enable field is set—reinitializes clients, calls `startStreaming`, and sends notification code `3`. The decompilation does not establish that every notification code `2` invokes this exact second stage, so the relationship should not be treated as proven without the notification dispatcher.

This gives us a useful instrumentation target if we later diagnose clock/USB interruptions: count feedback descriptor errors, observer lock transitions, and stop/restart events separately. It does not justify changing our adaptive clock or queue behavior after the clean one-hour run.

## Ghidra pass 10: stream recovery is a client-mediated two-stage protocol (2026-10-13)

I traced references to `stcNotifyStreaming` (`0x1686c`), `onDispatchStreaming` (`0x13eae`), and `handleStreamBroken` (`0x16924`) with a read-only Ghidra headless script against the existing `NumarkNS6Audio` project:

- `stcNotifyStreaming` forwards its event code to `PGKernelDeviceNUMARKNS6::messageToClients`. The feedback-failure path sends code `2` after stopping the stream.
- The exported `ASClientNUMARKNS6::method_CallDriver` and `AudioUserClientNUMARKNS6::userDispatch` each expose command `0x1f` as `handleStreamBroken(..., 1)` and command `0x20` as `handleStreamBroken(..., 2)`. Stage 1 returns a result to the caller; stage 2 can rebuild the configuration, reinitialize clients, restart streaming, and publish code `3`.
- `onDispatchStreaming` simply passes the client's `PT_Dispatch` through `dispatchFunction`; that dispatch also exposes explicit `resumeStreaming` and `suspendStreaming` commands. This is the client-facing control surface around the lower-level USB stream lifecycle.

This confirms that the kext has a two-stage recovery protocol available to its user-space client, which is more deliberate than a USB callback restarting the stream by itself. It still does not prove that every event code `2` causes the HAL to issue both commands: that decision lives in the separate user-space HAL binary, so the kext alone cannot close that last link. A follow-up pass should inspect the corresponding HAL notification handler before treating recovery as automatic.

## Ghidra pass 11: HAL automatically chains both recovery stages (2026-10-13)

I inspected the original x86_64 Ploytec HAL binary in a separate read-only Ghidra project and traced `PGOSXDevice::KernelDriverNotificationHandler` (`0x35b50`). It reads the `PT_StateChange` event code at offset `+4`; event `2` takes the `STC_STREAM_BROKEN` path and logs that event. The handler then calls `DriverConnection::callDriver(..., 0x1f)`. Only when that call returns with its success bit set does it wait 100 ms and call `DriverConnection::callDriver(..., 0x20)`.

Cross-referencing those selectors against the kext dispatch closes the gap left in pass 10: `0x1f` enters `PGDevice::handleStreamBroken(..., 1)`, and `0x20` enters `(..., 2)`. Stage 1 can stop/unconfigure the old stream and reports whether this caller won the recovery handoff; the HAL uses that result to gate stage 2. Stage 2 waits another 100 ms in the kext, reapplies the current configuration, reinitializes clients, restarts streaming when streaming remains enabled, and publishes streaming-state notification `3`.

This is direct evidence that the vendor HAL automatically handles the kext's stream-broken notification with a gated two-stage recovery sequence. It does not show that every audible glitch follows this path, nor that our CoreAudio implementation should copy the Ploytec recovery design unchanged. The relevant artifacts are `/private/tmp/ns6-ghidra-hal/hal-recovery-pass15.txt` (HAL handler) and `/private/tmp/ns6-ghidra-pass2/recovery-xrefs-pass13.txt` (kext dispatch and stages).

## Ghidra pass 12: Ploytec feedback-error threshold semantics (2026-10-13)

Tracing the kext's static feedback completion callback (`isocReadFeedbackCompleteStatic`, `0x19bd0`) shows that it increments `mIsocInFeedback.mErrorCounter` once for each ISO descriptor whose status is not `3` (the successful completion status used by this path). The observer then processes the completed feedback data. If the observer reports a lock/reacquisition event, `isocReadCompleteFeedback` clears the counter; otherwise the count is retained. When it exceeds `0x28` (40), the kext stops streaming and sends notification code `2` to clients instead of queuing another feedback read. Thus this is not a simple 41-consecutive-completions counter: it counts descriptor errors since the last observer lock/reacquisition indication.

Our `NS6USB.c` feedback reader currently increments `feedback_packet_errors` for failed transfer callbacks, failed packet descriptors, and failed read submissions. That metric is cumulative for the USB session; valid feedback does not clear it, and it does not itself request USB-audio recovery. A failed completion callback retries that slot, but an immediate `LowLatencyReadIsochPipeAsync` submission failure only increments the counter and returns. Since the reader queues two slots, one failed submission can leave the other slot running; if both initial submissions fail, no feedback read is requeued. Separately, if previously locked feedback becomes stale for more than two seconds, the output path drops the measured clock estimate and returns to nominal 44.1 kHz cadence, but it does not restart the transport. The output ISO path has its own recovery trigger: three consecutive transfer-completion failures (or an immediate submission failure) request a full USB transport restart.

The practical difference is specific: Ploytec escalates persistent trouble on the feedback input stream into a client-mediated stream rebuild, while our driver currently logs feedback errors and falls back from stale clock feedback without restarting. This makes a feedback-only recovery policy a candidate for a later controlled test, not an automatic change: the old threshold is tied to its observer's lock state and the full Ploytec HAL/kext lifecycle, so copying `>40` alone would not reproduce its semantics.

## Ghidra pass 13: `STC_STREAM_BROKEN` is a generic recovery notification (2026-10-13)

I decompiled the callers that publish streaming notification code `2`. The feedback-error threshold is only one source:

- `rtsIsoWriteComplete` publishes code `2` when the host's current USB frame has reached/passed the scheduled OUT frame. It logs the lateness, advances the next-frame target, stops streaming, and notifies clients.
- `rtsIsoReadComplete` publishes code `2` after its transfer-error counter exceeds `40` on the supported device paths, or when the reported channel configuration changes unexpectedly.
- The bulk and interrupt IN/OUT completion callbacks also stop an active stream and publish code `2` for failed completions (with an aborted-transfer status exception on the input paths).
- The feedback reader publishes the same code after its error counter exceeds `40`.

Therefore, the HAL's `0x1f`/`0x20` recovery sequence is shared across several failure classes. Seeing `STC_STREAM_BROKEN` or notification `2` alone cannot identify whether the cause was feedback, a late isochronous OUT request, an IN error, a configuration change, or another endpoint. Root-cause diagnosis requires the originating kext log/callback or equivalent per-endpoint telemetry. This also means the Ploytec recovery path is not evidence that our current feedback error count should directly trigger a whole-stream restart.

## Comparison with the current CoreAudio driver

The current implementation uses a different architecture, so the Ploytec worker thread and shared `AudioSM` ring should not be copied literally:

- The standard `kAudioDevicePropertyBufferFrameSize` setter in `NS6HAL.c` validates and stores the requested CoreAudio frame size, then notifies the host. `do_io` receives the actual runtime `frames` count for each `WriteMix` and enqueues that block directly. The callback frame count is recorded by Active Flow; the transport code does not itself use the standard property value as a queue target.
- A separate custom `NS6_DRIVER_BUFFER_PROPERTY` stores the Status app's persistent startup-buffer selection and requests a USB transport restart. It passes the selected value to `ns6_usb_set_startup_buffer_frames`. However, `NS6USB.c` enforces `NS6_STARTUP_MIN_PREFILL_FRAMES = 1024`; every custom option currently offered (49, 128, 192, 256, 512, and 1024 frames) is at or below that floor. Thus those choices all produce the same minimum 1024-frame startup prefill in the current code, even though changing the setting still restarts the transport.
- The USB task drains `NS6Transport` into variable-size 5/6-frame USB packets. Thus the app's submitted block size and USB packet size are separate quantities. Neither the standard frame-size property nor the custom startup-buffer setting is a continuously enforced target occupancy for the transport queue. After startup prefill, the steady path uses live queue availability in its clock-control/resampling decision. Active Flow reports the callback frame count observed at runtime; it is the appropriate value for seeing what the app is currently submitting.

One sizing difference called for a controlled latency experiment: the clock controller's default long-term occupancy setpoint is **4,096 queued frames**, about **92.9 ms at 44.1 kHz**. The Ploytec HAL's NS6 write-ahead base is computed from the requested block size: with a 44.1 kHz rate and the shown formula, it is about 508 frames for a 96-frame block and 1,120 frames for a 256-frame block. Those values are not directly equivalent to end-to-end latency, but they show that our queue target is substantially more conservative than the old HAL's calculated write-ahead base.

An offline A/B simulation now builds the same clock-control tests with 4,096 and 2,048-frame queue targets. At 2,048 frames (about **46.4 ms** at 44.1 kHz), both the 60-second rate-mismatch scenario and the 180-second conservative 44 kHz output-cadence scenario pass without approaching the 16,384-frame ring capacity. Their simulated peak occupancies drop from 5,361 to 3,313 frames and from 7,732 to 5,684 frames, respectively. This supports a hardware trial of the lower target but does not validate it under real CoreAudio scheduling; the production default remains 4,096 frames until that listening/underrun test is done.

The practical distinction is: the legacy Ploytec HAL uses the selected frame size as its recurring user-space processing block and includes that size in write-ahead/latency calculations; our driver accepts each host `WriteMix` block directly and uses live queue availability for steady transport. The separate Status startup-buffer setting currently has no differentiating effect among its offered sizes because of the 1024-frame floor. A status display should label the app callback size, startup prefill setting, live queue depth, and USB packet cadence separately instead of presenting them as one “buffer” number.

## Ploytec-era USB trace cross-check (2026-10-12)

I re-read `/Users/maurojunior/Downloads/ns6_full_10min.pcapng` with the Darwin USB fields and ISO descriptors, rather than relying on the earlier summary of URB totals. The capture metadata identifies macOS 10.14.6 / XHC20 and the existing technical report attributes the session to the proprietary Serato-era macOS driver. The pcap itself does not record which exact kext binary was loaded, so this is a macOS vendor-driver trace cross-check, not proof that this exact `NumarkNS6Audio.kext` produced every packet.

For endpoint `0x02` OUT, the capture has 150,028 completed URBs, each containing 32 isochronous descriptors. Every descriptor status was success. The individual payload lengths were:

| Payload | Frames at 12 bytes/frame | Count |
| ---: | ---: | ---: |
| 60 bytes | 5 | 2,339,303 |
| 72 bytes | 6 | 2,461,593 |

The URB totals correspondingly divide into 88,883 transfers of 2,112 bytes (176 frames) and 61,145 of 2,124 bytes (177 frames). The 60-second portions are highly consistent, around 2,646,111–2,646,114 frames per minute. This directly confirms the 5/6-frame packet shape predicted by `PGIsocRB::initWrite_Std`; it is strong agreement between decompiled design and observed macOS traffic.

There are two caveats:

- Dividing the captured 26,466,073 output frames by the pcap's 600.111-second total span gives approximately **44,101.95 frames/s**; using the first-to-last completion timestamps gives about 44,102.18. Both are roughly 44–50 ppm above 44,100. This is an effective rate inferred from capture timestamps and packet lengths, not a calibrated measurement of the NS6 crystal. We should not describe this trace as proving exactly 44,100 frames/s; its stable packet distribution is the stronger result.
- Endpoint `0x81` completions contain four 3-byte ISO slots. Across 150,028 completions, the first slot is a data triplet (`2c2c2c`: 118,889; `2c2d2c`: 30,005; `2c2c2d`: 573; `2d2c2c`: 561), while the other three slots contain `aaaaaa`. All 600,112 feedback descriptors completed successfully. The Ghidra pass confirms that the NS6 kext path treats in-range first bytes as feedback and predicts through out-of-range `0xaa` slots. The capture still does not establish the exact timing/phase relationship between each report and each output packet, or prove which exact kext binary produced the trace.

The practical conclusion is that the NS6 macOS playback path does not require every isochronous OUT packet to be 66 bytes. It sends both 60- and 72-byte payloads successfully in this long trace. Ghidra shows the original NS6 driver routes EP `0x81` into its feedback observer and uses that observer to select packet-frame patterns. Only the exact per-interval correlation and capture-to-kext build attribution remain open.

## Ghidra pass 14: feedback uses a two-request pipeline over a four-slot ring (2026-10-13)

I traced `PGDevice::rtsIsocAudioRun`, `requestIOFeedback`, `initNextTransactionFeedback`, `IsocData::next`, and the completion callback:

- `IsocData::initForStream` resets four reusable `PGIsocRB` slots. `IsocData::next` advances through those four slots and refuses to return one whose use counter is still nonzero.
- Stream start submits **two** feedback reads, while ordinary audio IN and OUT each submit **three** initial requests. Each successful feedback completion schedules one replacement request, so the feedback pipeline maintains two reads in flight when the USB callbacks keep arriving on time; the four slots provide room to recycle the completed buffer while another request is pending.
- Each feedback read uses its ring buffer's configured ISO descriptor count. `PGIsocRB` initializes that count to four and expands it to 32 when the parsed endpoint `bInterval` is `1`. The NS6 trace's four descriptors per EP `0x81` completion match the default path, though the capture does not prove it came from this exact kext build.
- The callback processes the feedback result, may enqueue the replacement, and then calls `PGIsocRB::setCallbackDone` to decrement the completed buffer's use counter. If the device has already marked streaming inactive, the callback skips feedback processing/requeue and only retires the buffer.

The stop path is asymmetric: `stopStreaming` clears the streaming flag, cancels the audio IN/OUT and keep-alive rings, then aborts the feedback pipe, but it does **not** explicitly call `IsocData::cancel` on the feedback ring. The feedback callback's inactive-stream branch and `setCallbackDone` appear to be the intended retirement path for aborted in-flight reads; `stopStreaming(true)` then waits 100 ms before its stopped callback. The decompilation does not prove that every pipe-abort outcome delivers a callback or that all four ring slots are retired before teardown, so this is an observation to verify against IOKit runtime behavior—not evidence of a confirmed vendor bug.

This pass adds a queue-topology detail to the previous recovery findings. Feedback has less initial request depth than the audio rings and is replenished one completion at a time, but this alone does not prove feedback starvation or explain an audible glitch. A useful comparison in a future controlled capture would correlate the two outstanding feedback reads, callback timing, ring-slot reuse, and any output deadline misses; our current driver's feedback queue is architected differently, so the Ploytec depth should not be copied without measurement.

## Ghidra pass 15: feedback pipe selection is based on endpoint descriptors

`PGPipeDescriptor::scan` decodes the endpoint address/direction, transfer attributes, `wMaxPacketSize`, and `bInterval`. `PGInterfaceDescriptor::getIsocInPipe` selects the first endpoint on that interface which is IN, is isochronous, and has a payload size of at least 32 bytes. The pipe is then opened by matching endpoint address and transfer type against the interface's endpoint list. In the observed NS6 configuration that pipe is EP `0x81` with 64-byte maximum packets.

This means `0x81` is the selected feedback endpoint for this interface/configuration, not a universal hard-coded feedback address in the generic pipe selector. A different product or alternate setting can select another endpoint or no feedback pipe at all.

## Ghidra pass 16: `bInterval` controls four versus 32 ISO descriptors

The `PGIsocRB` constructor and `reinit` start with four ISO descriptors. If the parsed endpoint `bInterval` equals `1`, the code marks the request as microframe-based and multiplies both descriptor count and backing-buffer size by eight, yielding 32 descriptors. On USB 2.0 high-speed endpoints, `bInterval = 1` denotes a service interval of one microframe; eight microframes make one 1 ms USB frame. Combined with the ring cursor's fixed advance of four USB frame units, the 4-to-32 expansion is consistent with keeping a four-frame request window while representing each microframe. This interpretation follows the code and USB 2.0 interval rules; the pcap does not include the NS6 endpoint descriptor needed to confirm its `bInterval` directly. I checked for standard `GET_DESCRIPTOR` setup requests in the pcap and found none; the current IORegistry view exposes the interface endpoint count but not each endpoint's `bInterval`. [USB-IF USB 2.0 specification](https://www.usb.org/document-library/usb-20-specification)

There is, however, a useful cross-check from the capture cadence. The EP `0x02` audio OUT URBs contain 32 descriptors and recur every 4 ms; this matches the `bInterval = 1` expansion exactly. EP `0x81` feedback URBs contain four descriptors, the descriptor timestamps are spaced about 1 ms apart, and the URBs recur every 4 ms; on a high-speed USB 2.0 endpoint that pattern is consistent with `bInterval = 4`. This is a strong inference from the driver's descriptor-count rule plus the trace timing, not a direct descriptor read, and it remains conditional on the capture using the same Ploytec code path.

## Ghidra pass 17: startup feedback reads are scheduled 50 frame units ahead

`PGDevice::rtsIsocAudioRun` initializes the read, feedback, and write ring cursors to `getCurrentFrame() + 0x32` (50). It submits two feedback reads immediately. `IsocData::next` assigns each selected ring buffer the current cursor and advances that cursor by four; the asynchronous read receives that start-frame value and the ring buffer's configured descriptor count. Later, `initNextTransactionFeedback` also maintains a guard at least four frame units beyond the current USB frame before queuing another read.

This explains why “four descriptors” and “four frame units” travel together in the default path, while the `bInterval = 1` path uses 32 microframe descriptors over the same four-frame window. The `+50` value is in the USB frame coordinate used by the driver; this disassembly alone does not justify converting it to an exact wall-clock lead time on every controller. When feedback's frame cursor reaches or falls behind the current frame, `initNextTransactionFeedback` moves it to `current + 4` before asking for another buffer. Audio IN and OUT also begin from the `+50` base, with three initial requests each, whereas feedback begins with two.

With the capture cadence above, the queue depths correspond to about 12 ms of audio OUT requests (three 4 ms transfers) and 8 ms of feedback reads (two 4 ms transfers) in flight. The stream's initial `+50` frame offset is separate from those steady-state queue depths; it is not another 50 ms of queued audio data.

## Ghidra pass 18: the feedback parser consumes byte zero of each ISO packet

`isocReadFeedbackCompleteStatic` walks the completed ISO descriptors, reads the first byte at each descriptor's payload pointer, and advances to the next payload using that descriptor's reported length. It copies those byte values into `FeedbackStreamingStruct`; it does not pass the remaining bytes of each packet to the pattern observer. Therefore, for the observed 3-byte EP `0x81` payloads, the parser's feedback sample is specifically byte 0. This agrees with the capture analysis that identifies byte 0 as the newest value in the sliding triplet.

The observer then validates each extracted byte against its expected feedback range. In the ordinary path, out-of-range values are replaced by the next predicted pattern value while the history cursor advances. In full-frame mode, values outside a narrow ±2-frame window invalidate lock and fall back to the nominal eight-packet pattern. This is a resilient predictor around the selected byte, not a decoder that averages all three bytes in the report.

Together, these four passes clarify endpoint choice, descriptor expansion, schedule coordinates, and payload decoding. They do not establish that a feedback callback delay caused our driver's audible artifacts; a useful next measurement would need the endpoint descriptor's actual `bInterval` plus timestamped completion and output-deadline data from the same run.

## Ghidra pass 19: MIDI IN is armed with a queue, and error completions are asymmetric (2026-10-13)

The x86_64 Ploytec kext `NumarkNS6Audio` 3.3.11 has two MIDI-IN implementations visible in the disassembly, so their behavior should not be conflated:

- `PGDevice::onStreamingStarted` starts both the `USBMidiDevice` path and `AJ::streamingStarted`. The latter initializes 16 asynchronous reads through `AJ::bulkRead` on its MIDI-IN pipe (and uses the same async completion machinery for its paired MIDI-OUT pipe). On a successful IN completion, `AJ::inAsyncCompletionRoutine` invokes the selected input parser and immediately calls `bulkRead` to submit another read. On an unsuccessful completion, it only logs `kIOUSBTransactionTimeout` when that is the reported status, retires the completed request, and decrements its outstanding counter; it does not call `bulkRead` on that path. Thus a failed read is not directly replenished by that callback. Other pending reads remain independent, and the static analysis does not show an automatic retry from this failure branch.
- The separate `USBMidiDevice::start` marks MIDI active and submits three reads from its four-slot request-block pool. Its `readComplete` callback only marks the block free; it does not itself process data or requeue that block. The decompilation shows this path is started and stopped alongside audio streaming, but does not establish that it is the active source for the observed NS6 MIDI events. The `AJ` route is the clearer continuous parser/queue path in this binary.
- `PGDevice::onStreamingStopped` stops the USB MIDI device and `AJ::streamingStopped` aborts both pipes. This ties the vendor driver's MIDI transport lifetime to the audio streaming lifecycle: it is started when CoreAudio streaming starts and aborted when streaming stops.

This makes the user's “MIDI input waits for messages” theory plausible only in a limited way. An idle endpoint can simply leave asynchronous reads outstanding; silence by itself is not shown to reset audio or to be an error. But if an IN request completes with a timeout/error, this callback does not replenish that slot, so repeated errors could shrink the outstanding-read queue until another stream start. The code does not show that a MIDI-IN error triggers the audio recovery path or causes an audible glitch. We need timestamp-correlated USB completion status, number of pending MIDI reads, MIDI traffic, and audio transfer errors to establish a causal link. The observation is static analysis of the legacy Intel kext; it does not prove the same behavior exists in the current Apple Silicon driver.

## Ghidra pass 20: MIDI data delivery is separate from audio recovery (2026-10-13)

I followed the successful MIDI-IN completion beyond the USB callback to the client-facing queue:

- The selected `inACR_*` parser calls `MidiIO::putFrame` with the received bytes. When bytes were accepted, it signals `MidiUserClientNUMARKNS6::triggerClient`, which only signals a semaphore. That is the event-delivery wake-up to the MIDI client; the parser does not call `stopStreaming`, restart an audio pipe, or manipulate the audio frame/clock state in the inspected path.
- The bulk MIDI reader maintains 16 `BulkRB` slots. `AJ::getIn` selects one free slot; `AJ::bulkRead` zeroes its buffer, submits an asynchronous read, and increments the outstanding-operation counter. Successful completion parses the bytes and submits a replacement read. A failed completion releases the slot and decrements the counter but does not requeue it. This is a queue-depth degradation path, not an explicit audio recovery path.
- `PGDevice::onStreamingStarted` is the common lifecycle hook: it starts `USBMidiDevice` and then `AJ::streamingStarted`; the latter selects a parser from the device/configuration and arms the 16 reads. `onStreamingStopped` stops the MIDI helper and aborts the `AJ` IN/OUT pipes. So MIDI transport is coupled to the *lifetime* of the audio stream in this legacy driver, while the data parser's successful-event path remains separate from audio scheduling.

The most defensible reading of the “MIDI waiting” theory is therefore: **missing MIDI messages alone are not shown to harm audio; repeated MIDI-IN transfer errors could silently shrink the receive queue, but the inspected code does not connect that condition to audio glitches.** A new capture should distinguish an idle request that remains pending from one that completes with timeout/error, and correlate the latter with audio endpoint completions and the USB LED. This pass is based on static x86_64 disassembly and does not establish the runtime's exact timeout behavior or prove that the current ARM64 driver has the same coupling.

## Ghidra pass 21: stream shutdown stops MIDI first; duplicate stop calls are guarded

I decompiled `PGDevice::stopStreaming(bool)` (`0x15c16`), `PGDevice::onStreamingStopped` (`0x12130`), `USBMidiDevice::stop` (`0x2710e`), `USBMidiDevice::start` (`0x2706c`), and `AJ::streamingStopped` (`0x02ee0`) in the existing read-only Ghidra project:

- `stopStreaming` calls `USBMidiDevice::stop` before clearing the streaming flag and aborting/cancelling the audio and USB pipes. It then calls `AJ::streamingStopped`, waits 100 ms when requested, and invokes `onStreamingStopped`; that callback calls both MIDI stop routines again.
- The apparent duplicate calls are guarded and idempotent. `USBMidiDevice::stop` checks its active byte, clears it before aborting the MIDI IN/OUT pipes and cancelling used request blocks, and returns immediately on a second call. `AJ::streamingStopped` likewise checks and clears its active state before aborting its two pipes, so the later callback does not repeat that abort.
- On startup, `onStreamingStarted` starts `USBMidiDevice` and then calls `AJ::streamingStarted`; the latter initializes its parser and posts 16 asynchronous bulk reads. The secondary helper starts three MIDI IN reads from its pool.

This gives a concrete vendor lifecycle reference for interpreting MIDI aborts around audio-stream shutdown: stopping audio intentionally stops both MIDI paths and aborts their outstanding requests; some aborted completions are therefore expected during teardown. The double call itself is not evidence of a bug because both paths have active-state guards. This static x86_64 kext analysis does not show that the current Apple Silicon driver follows the same ordering, nor does it explain a preceding `kIOReturnNotResponding`/pipe-stall or establish that MIDI teardown caused audio underruns. The user's 21:27–21:29 runtime observation should be treated as a stream stop/restart capture, not as a continuous-playback Mixxx MIDI test.

## Ghidra pass 22: stop is a shared lifecycle path, not a unique “app closed” event

I traced every reference to `PGDevice::stopStreaming` (`0x15c16`) and decompiled `PGKernelDeviceNUMARKNS6::cancelMidiRB` (`0x1e5a6`) plus `USBMidiDevice::requestMidiIn` (`0x270c0`):

- `stopStreaming` is reached from normal lifecycle/configuration paths (`suspendStreaming`, `setSelectors`, `onPrepareNewConfig`, `unconfigureDevice`, `usbApplyInterfaceSelector`, `terminate`, and `systemSleep`) **and** from error callbacks such as feedback failure, late isochronous OUT scheduling, audio IN errors/configuration changes, and bulk/interrupt IN/OUT errors. Therefore a stream stop in the Ploytec path is not by itself proof that the application requested closure; the initiating call site matters.
- Before stopping the audio worker and its rings, `stopStreaming` invokes `USBMidiDevice::stop`. That helper aborts MIDI IN/OUT pipes and walks up to four request blocks for each direction. For every non-null block still marked in use, `cancelMidiRB` sets a cancellation flag, sleeps **50 ms**, and clears the flag only if the block is no longer marked in use. Those waits are sequential in the decompiled loop; the theoretical code-path sum is up to eight 50 ms waits, though this is not a measured shutdown time and not every slot must be active. `stopStreaming(true)` also has the separate 100 ms post-abort sleep already documented in pass 9.
- `requestMidiIn` submits a read only while the helper's active flag is set and an IN pipe exists. Thus after the stop flag clears, this path will not replenish an input request; the stream-start hook re-arms reads later.

This confirms that the vendor driver intentionally couples MIDI request cancellation to stream shutdown, and that error-driven stream recovery uses the same shutdown routine as ordinary suspend/close. For our 21:27 observation, the user's report establishes that Mixxx was closed and the NS6 LED went out and returned; the old kext's call graph shows why the same visual stop/restart symptom could also be reached through an error path, but cannot classify the current driver's event. The 50 ms cancellation wait is a Ploytec implementation detail, not a recommendation to copy into our driver. Exact trigger identification still requires the current ARM64 driver's own stop/restart reason telemetry or USB/IOKit logs around the transition.

## Ghidra pass 23: the secondary MIDI helper appears unbound; AJ owns the concrete pipes

I followed the helper's request-block lifecycle and then checked its configuration caller:

- `USBMidiDevice`'s constructor initializes its MIDI IN/OUT pipe pointers to null. `PGDevice::findInterfacesInConfig` calls `AJ::assignPipes` with the device, which selects concrete interrupt/bulk MIDI pipes for AJ. It also calls `USBMidiDevice::assignPipes`, but the analyzed function body is an empty return and does not assign either helper pipe.
- `USBMidiDevice::configurationDone` allocates its four IN and four OUT request blocks only when the respective pipe pointer is non-null. `start` sets its active flag and tries three `requestMidiIn` calls, but each request checks for a non-null IN pipe before selecting a block and submitting a read. In the traced NS6 configuration path, the pointers were not populated, so the source does **not** establish that those three helper reads are actually submitted.
- The concrete AJ path is different: `AJ::assignPipes` obtains the selected interface's bulk OUT pipe and interrupt IN pipe, falling back to bulk IN when needed. `AJ::streamingStarted` configures the parser and submits 16 bulk reads. This is the path with demonstrated pipe assignment and the successful event parser described in passes 19–20.
- The helper's asynchronous `readComplete` ignores the completion status and only marks its `MidiRB` free; it neither parses MIDI bytes nor reposts a read. Its `read` call binds that callback directly. This behavior is consistent with a dormant/unbound auxiliary path in this NS6 configuration, not a second proven event stream.

This corrects the tentative wording in pass 19 that the helper “submits three reads”: its `start` loop requests three reads conditionally, but the traced pipe assignment leaves the conditions unsatisfied. The reliable static conclusion is that AJ owns the configured MIDI pipe path; whether any other runtime code patches the helper pointers remains unproven. Consequently, the current driver's post-close MIDI recovery logs should not be explained by assuming the old kext had two live receive queues competing for the same endpoint.

## Ghidra pass 24: identify which error callbacks can stop the vendor stream

I re-imported the original x86_64 `NumarkNS6Audio` binary into Ghidra 12.1.4
and traced the stop calls from the completion callbacks, rather than grouping
all input errors under “MIDI/USB.”

- The concrete MIDI path is `AJ::bulkRead` → `AJ::inAsyncCompletionRoutine`
  (`0x3d64`). On a failed completion it only logs the timeout status when the
  status equals `kIOUSBTransactionTimeout`, releases the `BulkRB`, and
  decrements AJ's outstanding-operation counter. Only the success branch
  parses data and submits a replacement read. There is no call from this
  callback to `PGDevice::stopStreaming`, `handleStreamBroken`, or the feedback
  clock path. Therefore a failed AJ MIDI read can shrink MIDI receive depth,
  but this callback does not itself stop or restart audio.
- The callbacks that do call `stopStreaming` are distinct `PGDevice` audio
  request handlers: `bulkInpEvent(AudioBulkIRP*)`,
  `bulkOutEvent(AudioBulkIRP*)`, `interruptInpEvent(AudioInterruptIRP*)`, and
  `interruptOutEvent(AudioInterruptIRP*)`. Their inputs are audio IRP types;
  seeing a stop call in one of them is not evidence that AJ's MIDI completion
  took that path. They mark/report `STC_STREAM_BROKEN` for applicable errors.
- The common stream stop still shuts down the MIDI helpers as part of teardown.
  That means MIDI requests may be aborted *because* the audio stream is
  stopping. A MIDI error logged at the same time as an app-close transition
  could consequently be a teardown side effect; timestamp proximity alone
  cannot establish that it initiated the stop.

This narrows the Ploytec comparison: the inspected binary has no direct
MIDI-IN-error → audio-stop edge on the live AJ path. It does have audio IRP
error → stream-stop edges, after which MIDI is torn down with the stream. This
is static evidence for the legacy Intel kext only; it does not establish the
cause of the Apple Silicon driver's LED blink. To establish that, the live
driver needs a same-timestamp reason for each pause/recovery/USB-presence
transition and a distinction between its MIDI completion errors and audio
transport callbacks.

## 0.2.143 idle LED capture: delayed completion handling and long async submit calls (2026-10-14)

The user confirmed this capture was recorded with 0.2.143 installed. The saved
`ns6-idle-led.log` filter omitted the new `USB OUT slow cycle` message, so the
matching interval was queried from the macOS unified log. The capture covered
roughly 00:05:20–00:06:55 while the stream reported `paused=1` and the transport
was still submitting idle PCM:

- 152 output cycles exceeded the diagnostic's 5 ms threshold. Every matching
  event had `paused=1`, `result=0x00000000`, and no USB transfer/packet error.
- `LowLatencyWriteIsochPipeAsync` took as long as **1,409,365 μs (1.409 s)**
  wall time while the calling thread accumulated only **43 μs** of CPU. Other
  slow calls included 595 ms, 207 ms, and 177 ms. The USB API returned success
  even after these long waits.
- The time from entry to the output completion callback until the next fill
  began reached **316,668 μs (316.7 ms)**. The fill itself remained short (up to
  4.7 ms wall time), so the delay is before the PCM fill; this measurement alone
  does not identify which operation inside the completion handler accounts for
  it.
- The output callback-gap telemetry reached **1.44 s**. Feedback callback
  cadence reached **253.9 ms**, consistent with both completion paths being
  delayed while sharing the worker's run loop. This correlation does not prove
  whether the delay originates in the run loop, logging/handler work, the IOKit
  async-submit call, or a lower USB-host scheduling layer.
- USB transfer errors, packet errors, and short packets stayed at zero. The
  reported underruns remained at 6 events / 32 frames across the captured
  health summaries rather than increasing during the idle portion.

This is the first direct runtime evidence of two distinct delay points during
idle operation: callback-to-fill handling and the nominally asynchronous IOKit
submit call. The next diagnostic should time the completion handler's internal
stages and CPU time separately, especially its logging/telemetry section, while
retaining the existing submit wall/CPU pair. Avoid attributing the LED blink to
USB packet errors: none were recorded in this run. The capture does show that
the transport callbacks and submissions can be delayed severely despite
successful IOKit return codes.

## 0.2.144 repeat Opera-close capture: delays reproduce on both cycles (2026-10-14)

The next capture was made with 0.2.144 installed after the user opened and
closed Opera twice. Unified-log records cover 00:11:24–00:12:18 and identify the
new build by its completion-handler diagnostics:

- There were 94 `USB OUT slow cycle` records in the interval; all were in
  `paused=1` and returned `0x00000000`.
- The slowest `LowLatencyWriteIsochPipeAsync` call took **391,646 μs (392 ms)**
  with **398 μs** of thread CPU. Other examples were 177 ms, 135 ms, and 89 ms.
- The completion-handler pre-submit interval reached **115,045 μs (115 ms)**,
  with only **258 μs** of thread CPU. The gap between completion callbacks
  reached **457,868 μs (458 ms)**. This confirms a real wait/descheduling period
  in the handler path in addition to the time spent inside the submit call.
- The saved live-stream file omitted the new diagnostic names, but the unified
  log contained 5 detailed completion-handler entries and 94 slow-cycle entries.
- USB transfer and packet errors remained zero. The health counter stayed at
  **24 underruns / 125 frames** through the captured interval; it did not rise
  during the idle cycles.

The submit-side delay is reproducible across the 0.2.143 and 0.2.144 captures.
To separate its cause from callback work, the next build times the callback-gap
logging call and the periodic health/telemetry logging block independently,
including wall and thread-CPU time. This still does not prove that either delay
caused a particular physical LED blink, but it rules out USB transfer-error
recovery and heavy PCM filling as explanations for these captured stalls.

## 0.2.145 Opera-close capture: per-gap logging stalls the USB callback (2026-10-14)

The user installed 0.2.145 and repeated opening/closing Opera. Unified-log
records from 00:15:45–00:17:35 contain 67 slow-cycle entries, 83 callback-gap
entries, and 3 instrumented completion-handler entries. Every slow cycle was
still `paused=1` and had a successful IOKit result.

The most diagnostic handler entry measured **56,980 μs (56.98 ms)** from
completion entry to the next submit, with **134 μs** of thread CPU. Its callback
gap `os_log` call alone took **56,968 μs (56.97 ms)** and **127 μs** of CPU. This
accounts for essentially the entire measured handler delay: the detailed log
call itself is blocking the USB event thread and can amplify the LED/callback
instability. The periodic health/telemetry block was not involved in these
slow-handler samples.

The IOKit submit stall remains a separate issue: the longest call in this
capture was **131,401 μs (131 ms)** wall time and **142 μs** CPU. The fill took
only 3 μs wall/CPU in that event. There were no USB transfer or packet errors,
and the underrun counter remained 24 events / 125 frames during the interval.

The next build removes per-event callback-gap, slow-cycle, and handler logging
from the timing-sensitive path while retaining the existing periodic aggregate
metrics. This tests whether diagnostic logging itself is causing the repeated
callback stalls; it does not assume that this alone explains the independent
long IOKit submit calls.

## 0.2.146 Opera pause/close capture: MIDI endpoint failures alongside delayed audio callbacks (2026-10-14)

The user reported frequent USB LED flashing and audible audio glitches with
0.2.146 installed. The saved `ns6-idle-led.log` covers approximately
00:22–00:23 and does not include MIDI messages in its live-stream predicate;
MIDI failure counts below came from the accompanying unified-log inspection.

- Audio health stayed at **24 underruns / 125 frames**, with zero USB transfer
  errors, packet errors, or short packets. The count did not increase in this
  capture, so it does not document a new software-counted underrun coincident
  with the user's audible glitches.
- Clock feedback stayed near 44.1 kHz. Around the Opera pause/close transition,
  the feedback reader accumulated additional short/out-of-range samples while
  the PCM queue reached zero; feedback polling continued.
- Aggregate timing reached **57.8 ms** for
  `LowLatencyWriteIsochPipeAsync`, **87.3 ms** for output frame-to-callback
  dispatch lag, and **58.2 ms** for feedback frame-to-callback dispatch lag.
  These are host-side delays despite successful OUT/feedback transfer status.
- MIDI IN logged `kIOReturnNotResponding` and `kIOUSBPipeStalled` failures in
  bursts around the audio pause and Opera client removal. The four-read queue
  reached **0/4** in flight, then the recovery worker cleared the input-pipe
  stall and restored reads. Thus the LED observation coincides with actual MIDI
  transport interruptions, not only delayed audio telemetry.

Source inspection shows audio isochronous OUT and MIDI bulk IN/OUT use the same
interface-0 `IOUSBInterfaceInterface` object and its async event source/run loop.
MIDI stall recovery runs on a separate writer thread and clears the MIDI input
pipe specifically; it does not call the audio restart path or clear the audio
OUT pipe. The shared callback/run-loop domain makes MIDI completion/error
handling a plausible contributor to delayed audio callback servicing, while
the pipe-specific recovery and unchanged audio error counters argue against
claiming a direct audio-transfer failure. The log does not establish that MIDI
errors caused the audible glitches or the LED flashing.

Next diagnostic: capture a unified log that includes MIDI completion/recovery,
audio callback/submit timing, audio health, and exact user-noted LED/glitch
timestamps in one interval. If a new build is warranted, first reduce the MIDI
recovery log volume to periodic aggregates while retaining counters and
recovery outcomes; this tests the shared-run-loop/logging hypothesis without
changing endpoint recovery behavior.

## 0.2.147 diagnostic change: throttle MIDI error logging

To test whether synchronous MIDI error logging in the shared interface run loop
amplifies callback delays, error detail is now emitted at most once per five
seconds globally. Per-slot/per-class error counters, pipe-stall clearing,
retries, and read resubmission are unchanged. This is a diagnostic isolation
change, not a claim that logging caused the audio glitch. The MIDI recovery
unit tests and full `make test` suite pass; package creation and code-signature
verification also pass. Hardware playback and LED behavior remain unverified
until the user tests 0.2.147.

## 0.2.147 first live capture: underrun counter rises after stream resume (2026-10-14)

The installed driver bundle reports version 0.2.147. The supplied
`ns6-idle-led.log` covers 00:27:40–00:29:23, but its predicate excludes MIDI
error/recovery messages, and the live stream reports 68 dropped messages.
Therefore this capture cannot verify whether the new MIDI error-log throttle
was effective.

- Audio health started at **8 events / 41 frames**, rose to **9 / 47** by
  00:27:59, and remained there while paused. At 00:29:14.293 Opera resumed the
  audio stream; by 00:29:22.287 the count had risen to **27 / 141** (18 new
  events and 94 frames in about eight seconds).
- USB transfer errors, packet errors, and audio OUT short packets remained at
  zero. Thus the health-counter increase is a host-side queue/underrun signal,
  not a reported failed USB OUT transfer.
- Feedback polling continued near 44.1 kHz, though cumulative feedback short
  packets and out-of-range samples rose during this longer capture. The audio
  PCM queue was empty while paused and refilled after the stream resumed.

This is a new measurable audio problem on 0.2.147 around Opera's resume
transition. It does not establish that the user heard each counted underrun,
or that MIDI recovery caused them. The next capture must include MIDI error
and recovery messages in the same log stream; this one can only establish the
audio-side timing and counters. Do not treat the current result as evidence
that throttling MIDI logs fixed the physical LED flashing or audible glitches.

## 0.2.147 silent Opera comparison: no new audio underruns while idle (2026-10-14)

The user then captured approximately 00:31:59–00:33:54 with Opera open but no
audio playing. The stream remained in silence/paused state with the PCM queue
at zero. Opera CoreAudio clients were added and removed, but the capture
contains no `audio stream resumed` transition.

- The health counter remained at **28 events / 147 frames** throughout this
  capture; no new underruns, USB OUT transfer errors, packet errors, or OUT
  short packets appeared.
- Feedback polling continued around 44,098.3–44,099.8 Hz. Its cumulative
  short/out-of-range feedback counts increased, but this did not coincide with
  a rise in the audio underrun counter during the silent interval.
- The capture filter still excluded MIDI recovery messages and reported 29
  dropped live-stream messages, so MIDI behavior cannot be compared here.

Together with the earlier resume capture, this narrows the measurable audio
issue to the active-flow/resume transition: silent Opera presence and its
CoreAudio client churn did not add underruns, while the prior capture added 18
short underruns after audio resumed. It does not yet identify the cause of the
physical LED behavior because the log contains no timestamped LED signal, nor
does it show whether MIDI endpoint recovery coincided in this silent run.

## 0.2.147 short MIDI-only capture: no logged MIDI errors, but audio queue active (2026-10-14)

The user captured 00:36:59.638–00:37:26.262 with MIDI active and no audible
program material. The filtered file contains 30 driver records and no dropped
message markers. No MIDI input failure or recovery message appears in this
27-second interval. Since event-success messages are not logged, this does not
prove that MIDI packets/events were received; it only shows no logged MIDI
transport error/recovery during the short sample.

The audio-health queue was **4,257**, **4,663**, then **4,025 frames** in the
three summaries. This is unlike the previous silent-Opera capture, where the
queue stayed at zero, and means the driver still saw an active CoreAudio PCM
flow (or retained queued PCM); the capture has no pause/resume transition to
identify the client. Therefore this is not yet a clean MIDI-only/USB-idle
baseline despite no audible playback. The underrun counter stayed at 48 / 250
frames during the capture, with no USB OUT/packet errors. Clock feedback was
near 44.1 kHz. A longer MIDI-only test should first confirm the active CoreAudio
client or wait until the queue reaches zero, then capture MIDI and health logs
together.

The user clarified that the earlier 00:36 capture still had a track loaded in
the deck. A new capture at 00:39:06.616–00:39:38.360 was made with no audio
playing and only NS6 controls being moved. It contained no MIDI error/recovery
logs, no dropped messages, and the audio-health count remained **48 / 250**.
The PCM queue nevertheless remained around **3,775–4,464 frames**, so the driver
was still processing a silent/live CoreAudio flow; “no sound playing” should
not be interpreted as “no active audio client.” Clock feedback stayed at
44,100 Hz with no additional short/out-of-range counts in the shown interval.
This distinguishes the control-only/no-program-material state from the prior
track/resume test: no new underruns were measured during this short control
activity sample.

## Client-close LED transition: prioritize the audio idle payload (2026-10-14)

The user clarified the reproducible pattern: the NS6 USB LED stays steady while
Mixxx or Opera is open, whether the client is producing audio or silence. The
instability begins when the client closes and then persists. Sending MIDI
controls does not reproduce the blink, so MIDI traffic is no longer the lead
hypothesis for this symptom.

This points to the final CoreAudio client's stop transition. In `NS6HAL.c`,
when the final I/O client stops, the HAL pauses USB output and discards queued
PCM while keeping the USB transport running. In `NS6USB.c`, paused output is
then zero PCM (apart from the one-time standby prime at transport startup).
That differs from an open-but-silent client, which continues submitting live
PCM. The timing is consistent with an idle-payload/device-state interaction,
but does not prove causation.

Built diagnostic package `NumarkNS6-0.2.148.pkg` with
`NS6_IDLE_PCM_KEEPALIVE=1`. It keeps the existing one-LSB four-channel PCM
pattern active while output is paused, without changing active audio, MIDI, or
clock behavior. This is an A/B test of the final-client-close hypothesis, not
a confirmed fix. Hardware result is pending.

## Gregory's USB LED finding vs. our MIDI output shutdown path (2026-10-14)

Gregory's `NOTES/led_findings.md` separates two behaviors: with the ISO keepalive
still running, pausing host MIDI writes made the USB LED go dark; resuming MIDI
writes brought it back. A separate earlier flashing observation was later
attributed to an unpaced MIDI output loop. This makes the LED an activity
indicator in those tests, not a direct USB-link or audio-clock measurement.

The current driver has a potentially relevant output burst. The user-session
MIDI bridge recognizes the private SysEx heartbeat `F0 7D 4E 53 36 48 F7`;
after a received heartbeat goes quiet for 2 seconds, it sends a blackout sweep:
815 MIDI messages (CC 0..0x51 and Note Off 0..0x50 on channels 0..4), batched
into 63 UDP reports with no pacing in the bridge. The HAL-side USB writer then
drains the queue with successive `WritePipe` calls. This could create a brief
LED activity burst near client shutdown if the client had been sending that
heartbeat.

However, the installed Mixxx controller script and XML were searched and do
not contain that heartbeat signature, so the bridge timeout path is not yet
shown to run in the user's Mixxx case. The other blackout sweep is called by
`ns6_midi_stop()` during USB transport cleanup; ordinary final-client
`stop_io()` only pauses audio and discards PCM, and the existing lifecycle
analysis says it leaves the transport open. Therefore neither blackout path is
currently proven to explain Opera's close-time behavior or a persistent blink.
The remaining differentiating evidence is whether outgoing USB MIDI writes
occur at the observed blink, and whether the blink lasts only through a burst
or continues after writes stop. The 0.2.148 idle-PCM package does not test this
MIDI-output hypothesis and should be held pending that check.

The next package is a logging-only diagnostic, **0.2.149** (normal zero-PCM
idle configuration). It logs when the bridge first sees the private heartbeat,
aggregates host-to-device MIDI report/byte counts once per second, records a
heartbeat-timeout blackout and its report count, and records the count,
failures, and duration of the HAL transport-cleanup blackout. It does not add
delays, change packet contents, or alter the audio idle payload. The complete
`make test` suite and package build passed; the production USB work-interval
regression and ad-hoc code-signature checks also passed. This build is intended
to answer whether a MIDI output burst or a true transport cleanup coincides
with the close-time LED symptom. It has not been tested on hardware yet.

## LED blink correlates with MIDI bulk-IN pipe recovery (2026-10-14)

The user reports that every observed USB-LED blink coincides with MIDI pipe
stall/recovery messages. In the 01:06 capture, the MIDI input reader on bulk
endpoint **0x83** repeatedly entered recovery immediately before the reported
blink interval. `ClearPipeStallBothEnds` took **179,926 us** once; other clears
in the same burst completed in sub-millisecond time. A clear was requested
while **3 of 4** asynchronous reads were still in flight. The log then shows
only the triggering slot resubmitted, followed by recovery/resubmission of the
other three slots.

This sequence matches the IOKit SDK contract: `ClearPipeStallBothEnds` is
equivalent to `ClearPipeStall`, which clears the endpoint halt/data-toggle and
returns outstanding asynchronous transactions so their callbacks run. Our
current MIDI recovery handles the triggering read per slot, while the other
in-flight reads re-enter recovery as collateral callbacks. This can create a
burst of read cancellations and resubmissions around one endpoint stall. The
user's LED correlation makes EP 0x83 recovery the leading explanation for the
blink; it is not yet proven that the recovery call itself causes the physical
LED behavior or that it is the only cause.

The separate 0.2.150 prebuffer test addresses the short audio queue underruns
on flow resume. It does not change MIDI recovery and is not an LED fix. The
0.2.151 test build coordinates stall recovery endpoint-wide: it quiesces the
read callbacks, waits for outstanding requests to drain, clears EP 0x83 once,
and then restores the four read slots as a group. Expected callbacks from the
clear are suppressed instead of starting a second per-slot recovery. This is
hardware-unverified; the test should confirm MIDI controls recover, the LED no
longer blinks in response to the cascade, and no new audio errors appear.

## Opera close/reopen capture: resume underruns and short startup prefill (2026-10-14)

In the user's latest capture, the NS6 USB LED blinked during the tested interval.
At 01:13:02.201 the driver logged the final audio client pausing while the USB
clock continued with silence; `StartIO` resumed that client at 01:13:02.388.
At 01:13:02.440 the first new underrun was logged. By 01:13:05.416 the total
had risen from 65 events / 340 frames to 83 / 434, an increase of 18 events /
94 frames. USB transfer errors, packet errors, and short OUT packets remained
zero.

At the same 01:13:05 health snapshot, `WriteMix` reported a maximum 217.838 ms
callback gap. That spans the stop/resume transition; it is not evidence of a
217 ms scheduling stall while active. USB completion cadence averaged 4.0 ms,
with 13.791 ms maximum, and frame-to-callback dispatch lag peaked at 10.102 ms.
Thus the data points to a brief host-side queue deficit during restart, while
the USB transport itself continued without transfer errors. The user's LED
observation is temporally associated, but the logs do not measure LED state or
prove that the underruns caused the blink.

Code inspection found the resume prefill waits for at least 1,024 frames
(23.2 ms) before consuming queued PCM, even though the adaptive clock's normal
queue target is 4,096 frames (92.9 ms). The 0.2.150 test build aligns the
minimum resume prefill with the configured queue target. This adds buffering
only at initial start/resume; it does not change the steady-state clock target
or the USB packet format. The full unit suite, arm64 driver build, USB
work-interval regression, signature checks, and package creation passed. The
package is ready for hardware testing; playback and LED behavior remain
unverified.
