# Numark NS6 Audio OUT Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the first new, independently testable NS6 driver milestone: a four-channel 44.1 kHz CoreAudio output device with USB playback and feedback, without importing historical implementation code.

**Architecture:** Create a new source tree, keeping CoreAudio publication, USB session ownership, playback scheduling, and feedback policy in separate modules with narrow interfaces. Use the already proven AudioServerPlugIn plus user-space USB architecture. Do not copy historical implementation code. DriverKit is deferred because the user has no signing team/entitlements. A separate privileged service is conditional on an actual access failure during implementation, not a prerequisite for continuing.

**Tech Stack:** macOS AudioServerPlugIn, IOKit USB user-space APIs, C/C++ unit tests, Apple USB/CoreAudio frameworks.

**Spec:** `docs/superpowers/specs/2026-10-20-numark-ns6-modular-design.md`

## Global Constraints

- No implementation source or binary is copied or branched from a historical NS6 driver version.
- Evidence documents and existing worktrees are read-only inputs; this plan creates a separate `numark-ns6/` source tree.
- Audio OUT uses endpoint `0x02`; feedback uses `0x81`; capture `0x86` and MIDI `0x83`/`0x04` are out of scope.
- The CoreAudio real-time path performs no USB I/O, allocation, blocking, or worker waits. A device-wide client lifecycle is shared by future streams; output and input demand and data paths remain independent.
- Keep one USB session owner, but separate endpoint workers, queues, and recovery state.
- Build artifacts may be produced for review; do not install or replace the user's active driver as part of this plan.

## Review Focus

- **Transport ownership:** verify that the AudioServerPlugIn process can own the active NS6 USB interface; use a privileged helper only if a direct test proves it is required.
- **USB descriptor drift:** reject a device whose VID/PID or required interface/endpoint layout differs from the measured NS6 instead of claiming it.
- **Fractional frame scheduling:** test 44.1 kHz whole-frame packet cadence without assuming constant packet size.
- **Real-time safety:** stress the output producer while confirming callbacks never wait on USB or allocate.
- **Disconnect during I/O:** prove cancellation releases the interface and callbacks cannot access destroyed buffers.

---

### Task 1: Record the selected platform and module boundary

**Files:**
- Create: `numark-ns6/README.md`
- Modify: `docs/superpowers/specs/2026-10-20-numark-ns6-modular-design.md`
- Modify: `docs/superpowers/specs/2026-10-20-numark-ns6-audio-platform-decision.md`

**Interfaces:**
- Consumes: measured device identity and endpoint table in the design spec.
- Produces: an explicit platform decision. Existing working AudioServerPlugIn releases establish the architecture; their source is not reused.

- [x] **Step 1: Select AudioServerPlugIn plus user-space USB** based on the working prior driver architecture; keep all new code in `numark-ns6/`.
- [x] **Step 2: Defer DriverKit** because no Apple team or approved entitlements are available.
- [x] **Step 3: Record USB access risk accurately:** an unprivileged probe did not open the currently bound device; this is a process/ownership permission result, not evidence that the chosen architecture is infeasible. Resolve ownership in the USB-session implementation; only add a privileged helper if direct implementation testing requires it.
- [ ] **Step 4: Commit** the platform decision with the first implementation milestone.

**Verification:** platform selection is documented; no system driver was changed during this decision.

### Task 2: Define module contracts and test harness

**Files:**
- Create: `numark-ns6/audio-out/AudioOutputRing.hpp`
- Create: `numark-ns6/audio-out/AudioOutputRing.cpp`
- Create: `numark-ns6/audio-out/AudioPacketPolicy.hpp`
- Create: `numark-ns6/audio-out/AudioPacketPolicy.cpp`
- Create: `numark-ns6/tests/audio_out_tests.cpp`
- Create: `numark-ns6/CMakeLists.txt`

**Interfaces:**
- `AudioOutputRing::write(const float *interleaved, uint32_t frames) -> uint32_t` accepts available frames without blocking.
- `AudioOutputRing::read(float *interleaved, uint32_t frames) -> uint32_t` returns available frames without blocking; the caller handles silence fill.
- `AudioPacketPolicy::nextPacketFrames() -> uint32_t` distributes the nominal sample rate as whole frames across a configured number of service intervals.
- Feedback parsing and rate correction remain isolated in the later feedback module; the service interval is supplied from the active endpoint configuration, not inferred from transfer-completion counts.

- [x] **Step 1: Add ring tests** for wraparound, empty read, partial write, and full ring; assert ordering.
- [x] **Step 2: Add packet-policy tests** for 44.1 kHz cadence at 1 kHz and 8 kHz service rates, plus invalid zero-rate rejection.
- [ ] **Step 3: Run tests and record expected failures.**
- [x] **Step 4: Implement the bounded ring and pure nominal packet policy** that satisfy the tests.
- [x] **Step 5: Run the test target with sanitizers** and verify ordering, bounds, and long-term frame totals.
- [ ] **Step 6: Commit** the tested data-path primitives.

### Task 3: Implement USB session owner and audio OUT worker

**Files:**
- Create: `numark-ns6/usb/NS6DeviceSession.hpp`
- Create: `numark-ns6/usb/NS6DeviceSession.cpp`
- Create: `numark-ns6/audio-out/AudioOutWorker.hpp`
- Create: `numark-ns6/audio-out/AudioOutWorker.cpp`
- Create: `numark-ns6/tests/usb_session_tests.cpp`
- Create: `numark-ns6/tests/audio_out_worker_tests.cpp`

**Interfaces:**
- `NS6DeviceSession::open(const NS6DescriptorSet &) -> Result` validates the expected interface map and owns configuration/interface lifetime.
- `NS6DeviceSession::close() -> Result` cancels pending transfers, joins workers, then releases interfaces.
- `AudioOutWorker::start(NS6DeviceSession &, AudioOutputRing &, AudioPacketPolicy &) -> Result` owns only endpoint `0x02` submissions.
- `AudioOutWorker::stop() -> Result` cancels only its own pending OUT transfers.

- [ ] **Step 1: Add failing session lifecycle tests** for valid descriptors, invalid endpoint type, duplicate owner, transfer-submit failure, and detach during pending I/O.
- [ ] **Step 2: Add failing worker tests** for packet payload sizing, partial completion, cancellation, and queue-empty silence.
- [ ] **Step 3: Implement descriptor validation and one exclusive session owner.** Do not open MIDI or capture endpoints.
- [ ] **Step 4: Implement the OUT worker** using `AudioPacketPolicy` and only the bounded output ring.
- [ ] **Step 5: Run unit and sanitizer tests**; verify disconnect cannot produce use-after-free or leave a session claimed.
- [ ] **Step 6: Commit** the USB session and output worker.

### Task 4: Add the dedicated feedback reader

**Files:**
- Create: `numark-ns6/audio-out/AudioFeedbackReader.hpp`
- Create: `numark-ns6/audio-out/AudioFeedbackReader.cpp`
- Create: `numark-ns6/tests/audio_feedback_tests.cpp`

**Interfaces:**
- `AudioFeedbackReader::start(NS6DeviceSession &, FeedbackStateSink) -> Result` reads only endpoint `0x81`.
- `AudioFeedbackReader::stop() -> Result` cancels only feedback reads.
- `FeedbackStateSink::publish(const FeedbackObservation &) -> void` publishes validated observations to the packet policy.

- [ ] **Step 1: Add failing feedback tests** for valid, malformed, stale, and out-of-range samples using captured fixtures when available; label synthetic fixtures explicitly.
- [ ] **Step 2: Confirm the feedback byte interpretation** from USB traces/original-driver evidence before defining parser constants. Preserve unknown bytes in diagnostic output without treating them as valid timing.
- [ ] **Step 3: Implement reader and parser** with bounded in-flight requests and isolated recovery.
- [ ] **Step 4: Test feedback loss/recovery** without changing output queue policy or touching endpoint `0x02` state.
- [ ] **Step 5: Commit** the reader and validated parser.

### Task 5: Publish the output device through the selected CoreAudio API

**Files:**
- Create: `numark-ns6/coreaudio/NS6AudioDriver.hpp`
- Create: `numark-ns6/coreaudio/NS6AudioDriver.cpp`
- Create: `numark-ns6/tests/audio_driver_contract_tests.cpp`
- Modify: `numark-ns6/CMakeLists.txt`

**Interfaces:**
- The CoreAudio adapter exposes four output channels at 44.1 kHz and writes frames to `AudioOutputRing`.
- A device-wide lifecycle reference-counts active clients; stopping one client must not stop another active client's I/O. It is not owned by the output stream so input can share it later.
- Input will use a separate stream, capture ring, and worker; do not advertise it before its transport and decoder pass their own gate.
- USB/session ownership remains below the adapter and is not inferred from app process lists.

- [ ] **Step 1: Add failing adapter tests** for format/channel properties, two-client start/stop, silence on ring underrun, and first-client/last-client transitions.
- [ ] **Step 2: Implement the adapter** using the transport selected in Task 1; the real-time callback only copies/converts into preallocated ring storage and returns.
- [ ] **Step 3: Run all unit tests and inspect callback code** for allocation, lock waits, USB calls, or worker joins.
- [ ] **Step 4: Build the driver artifact** and verify its bundle identity and four-channel format without installing it.
- [ ] **Step 5: Commit** the CoreAudio output adapter.

### Task 6: Hardware validation and milestone package

**Files:**
- Create: `numark-ns6/docs/audio-out-hardware-test.md`
- Create: `numark-ns6/tools/audio-out-health-log.cpp`
- Create: `numark-ns6/packaging/README.md`

**Interfaces:**
- Health log reports completed/submitted USB transfers, non-silent frames, underrun frames, feedback validity, queue depth, callback gap, and recovery count.
- Package metadata records source revision, build settings, artifact hash, and the exact version under test.

- [ ] **Step 1: Add health-log checks** that distinguish USB transfer failure, queue underrun, and application-level audible glitch.
- [ ] **Step 2: Run a 30-minute hardware playback test** with the NS6 as the only new module; capture idle, steady playback, second-client join/leave, and disconnect/reconnect phases.
- [ ] **Step 3: Run the regression matrix** for first app open and Opera video while Mixxx plays; record symptom, timestamp, and logs without adding MIDI/input/extensions.
- [ ] **Step 4: Build a local milestone package** and verify package contents/hash. Do not install it in this plan.
- [ ] **Step 5: Commit** the test record and package recipe only if the acceptance gate passes.

**Acceptance:** all unit tests pass; hardware test has no growing underrun/drop counts; callbacks meet the no-blocking rule; both playback scenarios are reported by the user as clean; artifact hash and source revision are recorded. If audible glitches occur, stop and isolate this milestone before planning another component.

## Later sub-project plans

After Audio OUT passes and its interfaces are reviewed, create separate implementation plans for MIDI IN/OUT, Audio IN, and each optional extension. Each plan will test one subsystem without changing already accepted modules, then add one explicit integration task. This keeps independent protocols from being implemented or debugged in the same change set.
