# Numark NS6: modular design and implementation manual

**Status:** approved direction; implementation proceeds in the modular worktree
**Decision:** implement a new driver from an empty source directory. No historical driver source or binary is the implementation base. Existing repository notes and captures are evidence only.

## Goal

Build a macOS NS6 driver in small, independently testable components. Keep playback, capture, MIDI input, MIDI output, and optional extensions isolated so a regression can be attributed to the component that introduced it. Combine components only after each has passed its own test gate.

A implementação não copiará nem derivará código de versões anteriores. As descobertas de protocolo serão usadas como evidência e verificadas contra a nova implementação.

## Evidence ledger

### Directly observed or independently corroborated

| Finding | Evidence and limit |
|---|---|
| Device identity is Numark NS6, USB `15e4:0079`. | Read-only descriptor probe. |
| Interface 0 alternate 1 exposes audio OUT `0x02`, MIDI IN `0x83`, MIDI OUT `0x04`; interface 1 alternate 1 exposes feedback/status IN `0x81` and bulk IN `0x86`. | Physical-device descriptors and USB captures. Endpoint meaning must still be tested by each new component. |
| Audio playback uses isochronous OUT `0x02`; `0x81` carries feedback/status and is not the capture audio endpoint. | USB traces and original-driver analysis. |
| At 44.1 kHz, the original driver uses variable whole-frame USB audio packet sizes to represent the fractional frames per service interval. | Ploytec binary analysis; observed traces should establish the exact active mode and packet cadence before choosing a new scheduler. |
| Audio input is a stereo record mix on bulk IN `0x86`, not conventional PCM on the wire. | Synchronized QuickTime/USB capture and original Ploytec decoder analysis; envelope correlation 0.996. |
| That input is a hardware return of the NS6 Master/Record mix: software audio routed through the NS6 plus analog sources routed to Master are captured together. It is not a software copy of USB OUT and does not expose independent mic/line channels. | Numark/VirtualDJ routing documentation; synchronized decoded USB capture; user-confirmed QuickTime recording with computer playback and microphone. |
| Capture setup observed vendor request `0x49`: control IN returns mode `0x12`, followed by control OUT with `wValue=0x0032`, then bulk reads on `0x86`. | One synchronized capture. Treat as a protocol starting point, not a universal negotiation rule until repeated from a clean connection. |
| MIDI IN `0x83` and MIDI OUT `0x04` have different report framing. | Captures and original-driver analysis. Do not share a parser or trailer rule. |
| Original MIDI OUT reports were 42 bytes and ended in `0xE0` across 14,353 successful captured reports; MIDI IN reports were observed ending in `0x00`. | Multiple traces and Ploytec analysis. The meaning of the OUT control byte is unresolved; do not call it a keepalive or LED command without proof. |
| Closing one CoreAudio client and stopping the physical USB transport are distinct lifetimes in the original design. | Ploytec call-path analysis. Model application-client, stream, and USB-session state separately. |

### Known behavior, not a proven cause

- Adding audio input previously coincided with first-open playback glitches and continuous Opera playback glitches. Later tests also reported a clean first-open test after a change. The causal change has not been isolated conclusively.
- The right-deck fader produced unexpected MIDI reports and missed movements in later builds; it behaved correctly on an earlier installed build according to the user. This does not establish a hardware defect or identify whether report framing, read scheduling, bridge delivery, or mapping caused it.
- A line-connected external mixer caused interference that stopped when that mixer ran on battery. This points to the analogue source/routing in that test; it does not establish a driver defect.
- Null-IOProc startup, LaunchAgent/Activator, MIDI bridge, Status, firmware query, and reconnect behavior are separate experiments. None belongs in the minimal audio-out milestone.

## Architecture

Use one device/session owner for USB configuration and interface claims. It arbitrates access and device lifetime only; it must not combine the independent data paths. Each feature owns its endpoint handling, buffers, worker state, start/stop demand, and recovery policy.

```text
                ┌── shared device/client lifecycle ──┐
CoreAudio OUT ──┴─> output ring ──> OUT worker ──> 0x02
                           ▲           ▲
                    output demand   feedback 0x81

CoreAudio IN <── capture ring <── decoder <── IN worker 0x86
                          ▲
             NS6 hardware Master/Record mix
             (USB playback + routed analog sources)
                   separate capture demand and recovery

MIDI clients <── MIDI IN parser/queue <── MIDI IN worker 0x83
MIDI clients ──> MIDI OUT encoder/queue ──> MIDI OUT worker 0x04

Optional extensions: firmware query | Status | bridge | startup | reconnect
                         consume narrow device/session interfaces
```

The CoreAudio real-time callbacks must not perform USB I/O, block, allocate, or wait on a worker. `StartIO`/`StopIO` maintain device-wide client lifetime; `kAudioDevicePropertyIOProcStreamUsage` and stream-specific `DoIOOperation` identify which streams a client uses. Device/session lifetime is shared so an input client's stop cannot tear down a still-running output client. Stream demand and transport remain per direction: capture must not share playback rings, packet clocks, callbacks, or recovery state. MIDI IN and OUT likewise use separate framing and queues. A shared USB owner coordinates configuration and detach only; component workers must not silently start/stop each other's streams. See Apple's [AudioServerPlugInDriverInterface](https://developer.apple.com/documentation/coreaudio/audioserverplugindriverinterface) and [AudioHardwareIOProcStreamUsage](https://developer.apple.com/documentation/coreaudio/audiohardwareioprocstreamusage).

## Components and acceptance gates

### 0. Device/session foundation

Implement device discovery, descriptor validation, exclusive interface ownership, configuration/alternate-setting selection, cancellation, and detach handling. No PCM, MIDI parser, UI, helper agent, or firmware display in this step.

**Gate:** repeatable connect/disconnect and clean resource release; reject unexpected descriptor layouts safely; log interface/endpoint ownership and session transitions. Exercise failure and reconnect paths without leaving a claimed pipe or worker behind.

### 1. Audio OUT, the first playable milestone

Implement the first stream of a bidirectional-capable CoreAudio device: four-channel, 44.1 kHz output using `0x02` and a dedicated `0x81` feedback reader. The device owns client lifecycle once; the output stream owns its bounded producer/consumer ring and USB worker. Keep sample conversion, packet formation, and feedback-based scheduling testable without hardware. Do not expose a nonfunctional input stream yet.

The 44.1 kHz service interval cannot be represented by one constant integer frame count. Use whole-frame packet lengths driven by a validated cadence/feedback policy. Record requested/completed packet frame counts, queue depth, clock estimate, and callback gaps. Avoid naming USB audio feedback “MIDI Clock.” MIDI Clock, if ever supported, is MIDI real-time data and a different feature.

**Gate:** packet-pattern unit tests; ring wrap/full/empty tests; deterministic clock-policy tests; sustained hardware playback; no underrun/drop growth; no callback blocking. Establish the first clean installable package only after this gate.

### 2. MIDI IN and MIDI OUT

Add input and output as separate modules. IN owns `0x83`, its report parser, in-flight reads, ordering, and pipe recovery. OUT owns `0x04`, its encoder, 42-byte report construction, and output queue. Derive framing from captured reports and original behavior; preserve distinct trailers and filler/control-byte rules. Unknown bytes remain unknown until a controlled capture associates them with a specific action.

**Gate:** parser tests for messages split across reports, filler, malformed/truncated reports, and ordering; encoder tests against known report captures; hardware tests for each control and LED/output action; fader test with MIDI Monitor listening only to the NS6 input (disable output spy). Verify both movement response and stationary-noise behavior. Test one read/recovery variable at a time.

### 3. Audio IN

Add a distinct two-channel CoreAudio input stream for the NS6 hardware Master/Record return under the same device/client owner. The mixer internally returns its stereo program mix: software playback routed to the NS6 plus analog sources routed to Master. This is a hardware loopback through the NS6's mixer, not driver-side copying from USB OUT to USB IN. Negotiate the observed input mode, read bulk endpoint `0x86`, decode its packed bit lanes into samples, and publish through a capture-only bounded ring. Keep capture demand, decoder, timestamps/metrics, and recovery independent from audio OUT and MIDI. Do not claim isolated mic/line channels.

**Gate:** decoder fixtures based on captured blocks; bounds and malformed-block tests; compare a hardware recording against the NS6 input source; verify channel order, level, silence handling, that Master-routed USB playback and analog inputs are present in the return, sustained capture, and simultaneous playback + MIDI. Repeat the existing first-app-open/Opera test with capture absent and present while changing no other variable.

### 4. Extensions, one at a time

Keep extensions out of the data plane:

- **Firmware query:** a bounded protocol probe with parsing/validation separated from display. Record the raw reply and distinguish unknown firmware from read failure.
- **Status:** read-only snapshot of device/session state, firmware result, and current audio clients. Show current users from live client lifecycle events, not every process on the machine. Status must not claim pipes or alter stream state.
- **MIDI bridge:** transport decoded MIDI without reordering or blocking the USB reader; explicit backpressure/drop metrics.
- **Startup/Activator:** optional mechanism to keep the device ready. Test cold boot, first USB connect, sleep/wake, and no-client state independently from audio transport. Do not make it a hidden dependency of MIDI or playback.
- **Reconnect/recovery:** detect physical detach, cancel only affected workers, release session ownership, rediscover, and re-arm each requested component. A successful reconnect must not require restarting unrelated apps or CoreAudio unless that limitation is explicitly measured.
- **Installer/uninstaller:** package only completed components; install/remove its own files and launch items deterministically. Verify installed bundle/version against the built artifact.

Each extension has its own test gate, then a combined test with all previously accepted components.

## Incremental workflow

1. Create a new source target/module namespace; do not copy implementation files or binaries from historical versions. Keep old code and evidence untouched.
2. For each component, write its contract, endpoint ownership, lifecycle, failure behavior, and focused tests before connecting it to the rest.
3. Build and test that component alone. Record source revision, build configuration, artifact hash, hardware setup, test duration, logs, and result.
4. Integrate one accepted component at a time. Run the previous acceptance tests unchanged before adding the next one.
5. When a regression appears, compare the last passing integration with the first failing integration; revert or isolate only the newly added component. Do not jump to an arbitrary historical version as a substitute for locating the change.
6. Keep a known-good package for each milestone and label experimental packages clearly. Never install two competing NS6 drivers or claim success from a version string alone; verify the loaded bundle and executable artifact.

## Regression matrix

| Milestone | Required scenarios |
|---|---|
| Audio OUT | sustained playback; first app open; second audio client joins/leaves; Opera video while Mixxx plays; idle/standby; unplug/replug; sleep/wake |
| MIDI | each input control; fader moving and stationary; MIDI output; input while audio clients join/leave; read error and recovery |
| Audio IN | recording alone; recording + playback; recording + MIDI; first input client while playback is active; line connected/disconnected |
| Extensions | firmware success/failure; Status live client changes; bridge throughput/order; cold startup; reconnect without unrelated stream disruption |
| Integrated | repeat all prior scenarios with the exact final package and record artifact hash |

For each run, note whether the symptom is in playback, capture, MIDI, app responsiveness, or device lifecycle. A “glitch” report without that direction and timestamp is not enough to assign a root cause.

## Explicit non-goals for the first milestone

- Do not implement audio input, MIDI, Status, firmware display, bridge, startup agent, or reconnect in the initial playable audio-out package.
- Do not emulate or silently copy the original driver's complete architecture. Use original behavior as protocol evidence, then validate the smaller implementation.
- Do not infer that a zero underrun counter proves glitch-free audio; correlate counters with application audio and USB completion data.
- Do not interpret raw MIDI filler/trailer bytes without a controlled experiment.

## Open questions before implementation

1. What macOS deployment target and hardware support range should the new driver promise?
2. **Platform decision:** use the already proven macOS AudioServerPlugIn + user-space USB architecture. The new implementation remains clean-room and modular; previous releases and traces are protocol evidence, not source bases. USB ownership stays in one session module. Add a separate privileged service only if implementation testing proves the plug-in process cannot open the required interface.
3. Which exact CoreAudio format/channel mapping should the first audio-out milestone expose to applications?
4. Which signing/notarization and installation path is required for user testing?
