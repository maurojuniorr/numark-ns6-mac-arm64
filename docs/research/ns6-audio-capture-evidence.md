# NS6 audio-capture transport evidence

## Summary

The original Ploytec kext includes generic capture/audio-input code, but the available NS6-specific USB capture does not show a dedicated PCM input endpoint. Do not add a CoreAudio input stream from these findings alone.

## Hopper observations

- `PGConfigurationDescriptor::findAudioStreamingInInterface` and `PGInterfaceDescriptor::isAudioStreamingIn` are generic Ploytec descriptor routines.
- `PGDevice::bulkAudioRun` calls `PGDevice::setAJDMAInputChannels`; its pseudocode configures the input channel count and prepares bulk audio IRPs. `PGDevice::resumeStreaming` invokes `bulkAudioRun` together with `rtsIsocAudioRun` for some selected settings.
- These routines prove that the Ploytec framework contains input-capable code. They do not by themselves prove that the NS6 routes microphone/line audio through that path.

## NS6 USB capture observations

Artifact: `/Users/maurojunior/Downloads/ns6_full_10min.pcapng`, USB Darwin capture from macOS 10.14.6 on XHC20. TShark identified NS6 device address 13. Full-capture endpoint/type counts:

| Endpoint | Transfer | Direction | Captured transfers |
| --- | --- | --- | ---: |
| `0x02` | isochronous | host → NS6 | 300,056 |
| `0x81` | isochronous | NS6 → host | 300,056 |
| `0x86` | bulk | NS6 → host | 330,826 |
| `0x83` | bulk | NS6 → host | 4,270 |
| `0x04` | bulk | host → NS6 | 19,472 |

Endpoint `0x81` returned 12 bytes per captured transfer, with `2c2c2c` (44-frame feedback values) followed by placeholder `aaaaaa` values. This is feedback, not a PCM capture stream. Endpoint `0x83`/`0x04` carry MIDI. Endpoint `0x02` is playback. The remaining IN endpoint `0x86` is already identified by the project protocol notes as the waveform-display bulk endpoint; its captured 10,240-byte payload is highly structured and consists largely of `0x00`/`0x01` values rather than 24-bit PCM samples.

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

No alternate setting exposes another input endpoint. Any host capture would have to use `0x81` or `0x86` in a mode that was not identified in the available trace. The observed payloads identify `0x81` as clock feedback and `0x86` as structured waveform data during that session.

## Stop/go result

The capture does not contain a separate NS6 audio-input endpoint or recognizable PCM input payload. The Ploytec input functions are generic, and there is no NS6-specific descriptor/payload evidence tying them to the physical inputs. Therefore Task 1's stop/go gate is **not met**: do not publish an input device or treat `0x81`/`0x86` as capture.

To resolve the mismatch with the Windows panel's “Inputs: 2”, a useful next artifact would be a USB capture from the original Windows driver while recording known audio fed into the NS6's physical input. Compare IN payloads with the input silent and with a 440 Hz signal. That would show whether the Windows driver activates another alternate setting or repurposes an existing endpoint.

## Limits

- The pcap shows one captured configuration/session; it does not exhaust every possible alternate setting.
- The descriptors rule out an undiscovered alternate-setting endpoint on this hardware, but cannot rule out a different payload mode on an existing endpoint.
- The Windows panel's input count establishes software-reported channels, not the USB transport or that the NS6 sends those physical inputs to the host.

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
