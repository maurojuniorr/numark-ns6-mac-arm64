# NS6 Audio Capture Input Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Expose verified NS6 physical input audio to macOS as CoreAudio input streams without changing playback behavior.

**Architecture:** First derive the NS6-specific capture endpoint, transfer type, format, channel map, rate setup, and initialization from the original kext and primary USB evidence. Only after those facts are confirmed, add an independent USB capture worker, bounded producer/consumer queue, and CoreAudio input stream scope.

**Tech Stack:** C11, IOKit USB, CoreAudio AudioServerPlugIn, Makefile tests, Hopper REA evidence.

**Spec:** `docs/superpowers/specs/2026-10-06-ns6-audio-capture-design.md`

## Global Constraints

- Never treat EP `0x81` feedback, EP `0x86` waveform, or MIDI traffic as audio capture.
- Do not publish an input stream until the NS6-specific capture transport is verified.
- Keep capture I/O outside CoreAudio's real-time callback; callbacks consume only from a bounded queue.
- Preserve current four-channel output and MIDI behavior.
- On insufficient evidence, stop after the probe/report task without shipping a fake input.

## Review Focus

- Endpoint/interface mismatch: selector ignores non-capture descriptors and has table-driven tests.
- Disconnect/stall: capture worker stops/restarts independently and does not tear down playback.
- Queue overrun/underrun: bounded queue behavior is explicit, measured, and tested.
- Channel order/format: known test vectors prove conversion and physical-input mapping.
- Input-only versus duplex operation: USB configuration and stream start/stop tests verify both while output regressions remain covered.

---

### Task 1: Establish NS6 capture transport facts (stop/go gate)

**Files:**
- Create: `docs/research/ns6-audio-capture-evidence.md`
- Reference: `/Users/maurojunior/Downloads/NumarkNS6Audio.kext/Contents/MacOS/NumarkNS6Audio`
- Reference: `tools/ns6-probe.c`, existing USB descriptor/pcap evidence

**Interfaces:**
- Produces a checked-in evidence table: interface, alternate setting, endpoint address/direction/type, maximum packet/interval, stream format/channels, sample-rate request, activation requirements, and evidence source for every field.
- No product code is created in this task.

- [ ] **Step 1: Analyze Hopper callers and callees** for `PGDevice::bulkAudioRun`, `setAJDMAInputChannels`, `findAudioStreamingInInterface`, and the USB transfer completion routines.
- [ ] **Step 2: Inspect NS6 USB descriptors and existing captures** to identify capture candidates; explicitly exclude feedback/waveform/MIDI endpoints.
- [ ] **Step 3: Write the evidence table** with unknowns marked; distinguish observations from inference.
- [ ] **Step 4: Apply the stop/go gate:** continue only if all transport and format fields above are supported by NS6-specific evidence; otherwise report the exact missing evidence and stop before adding an input device.

### Task 2: Add pure capture descriptor, format, and queue tests

**Files:**
- Create: `coreaudio/NS6Capture.h`
- Create: `coreaudio/NS6Capture.c`
- Create: `coreaudio/NS6CaptureTest.c`
- Modify: `coreaudio/Makefile`

**Interfaces:**
- `bool ns6_capture_endpoint_matches(uint8_t direction, uint8_t endpoint, uint8_t transfer_type)` uses constants recorded in Task 1.
- `bool ns6_capture_convert_frame(const uint8_t *source, float *destination, uint32_t channels)` implements the verified sample format/channel map.
- Queue API is fixed-size and nonblocking: `bool ns6_capture_queue_push(...)`, `bool ns6_capture_queue_pop(...)`; exact frame capacity is selected from measured maximum USB transfer cadence and documented before coding.

- [ ] **Step 1: Write tests first** for descriptor acceptance/rejection, sample conversion and channel order, queue wraparound, full/empty behavior, and disconnect reset.
- [ ] **Step 2: Run `make -C coreaudio capture-test`** and verify each test fails for the missing implementation.
- [ ] **Step 3: Implement endpoint selection and pure frame conversion** from Task 1 facts.
- [ ] **Step 4: Implement the bounded SPSC queue** with atomics and no allocation/locks in callback-facing operations.
- [ ] **Step 5: Run `make -C coreaudio capture-test`** and verify all tests pass under warnings-as-errors.

### Task 3: Add independent USB capture worker

**Files:**
- Modify: `coreaudio/NS6USB.h`
- Modify: `coreaudio/NS6USB.c`
- Modify: `coreaudio/Makefile`

**Interfaces:**
- `bool ns6_usb_capture_start(void)` / `void ns6_usb_capture_stop(void)` manage verified capture independently of playback state.
- Completed USB transfers enqueue decoded frames into `NS6Capture` and record transfer errors; callbacks never wait for CoreAudio.

- [ ] **Step 1: Add a mocked transport lifecycle test** covering start, stop, error, and disconnect, confirming playback lifecycle calls are unchanged.
- [ ] **Step 2: Run the targeted lifecycle test** and confirm it fails before implementation.
- [ ] **Step 3: Add asynchronous reads** only for the Task 1-verified endpoint and configure sample rate using the verified control request.
- [ ] **Step 4: Recover stalls/disconnects** without stopping the existing output stream; reset the queue on restart.
- [ ] **Step 5: Run capture and existing USB tests**; verify no output regressions and log capture transfer health separately.

### Task 4: Publish CoreAudio input streams

**Files:**
- Modify: `coreaudio/NS6HAL.c`
- Modify: `coreaudio/NS6USB.h`
- Modify: `coreaudio/NS6HALTest.c` (or add the smallest existing-style HAL test)
- Modify: `coreaudio/Makefile`

**Interfaces:**
- Add stable input stream object IDs separate from output `STREAM_ID=3`.
- `do_io` serves input frames from the capture queue, zero-filling missing frames and recording capture underruns without blocking.
- Device stream/channel properties enumerate the verified physical input mapping only.

- [ ] **Step 1: Write HAL enumeration and input-read tests** for input scope, channel count, zero-fill, and simultaneous output.
- [ ] **Step 2: Run targeted tests** and verify they fail before implementation.
- [ ] **Step 3: Publish input stream IDs, format/rate/channel properties, and route `do_io` input callbacks to queue reads.**
- [ ] **Step 4: Ensure start/stop reference counting handles input-only, output-only, and duplex clients.**
- [ ] **Step 5: Run all tests and build the AudioServerPlugIn**; inspect the device in Audio MIDI Setup and confirm the input appears with expected channels.

### Task 5: Document and package the capture feature

**Files:**
- Modify: `coreaudio/README.md`
- Modify: package/build scripts only if required to include changed binaries

- [ ] **Step 1: Document physical input routing, format, rates, known limits, and verification status.**
- [ ] **Step 2: Run `make -C coreaudio test` and the package build.**
- [ ] **Step 3: Verify install/uninstall metadata and confirm output/MIDI are unchanged by the package update.**
