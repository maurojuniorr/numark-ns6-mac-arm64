# NS6 Audio Capture Input

## Goal

Expose the NS6's physical audio input channels to macOS applications as a CoreAudio input device while preserving the current playback and MIDI behavior.

## Evidence and current behavior

The macOS driver currently publishes output channels only. Its README states that the NS6 hardware inputs are not available as a macOS input device. Hopper analysis of the original Ploytec kext found generic audio-streaming-input discovery and capture-path routines, including `findAudioStreamingInInterface`, `setAJDMAInputChannels`, and `bulkAudioRun`. The latter configures input channels and prepares bulk-audio I/O. This evidence shows the original driver has capture-related machinery, but does not yet prove the NS6 endpoint number, packet format, or supported capture source layout.

## Design

First establish the NS6-specific capture transport from USB descriptors, the original kext's NS6 path, and available USB captures. Do not infer capture from endpoint `0x81` feedback or endpoint `0x86` waveform traffic. Once the endpoint, direction, sample format, channel count, rate setup, and required initialization are verified, add an independent capture pipeline to the CoreAudio driver and publish input streams on the NS6 device. Keep capture I/O and buffering separate from the playback real-time callback and preserve existing output behavior.

If the evidence does not identify a safe NS6 capture transport, stop at a documented probe/report and do not publish a nonfunctional input device.

## Requirements

- Verify the NS6-specific capture interface/endpoint and transfer type from primary artifact evidence.
- Determine sample format, channel ordering/count, sample-rate control, and any required initialization sequence before enabling capture.
- Do not treat feedback, waveform, or MIDI endpoints as audio capture.
- Add CoreAudio input streams only after transport facts are established.
- Keep input queueing independent of playback and never block the CoreAudio real-time callback on USB.
- Handle disconnect, USB errors, and stream stop/restart without disrupting output.
- Add tests for descriptor selection, format conversion/channel mapping, queue behavior, and error/disconnect handling.
- Document which physical inputs map to each published CoreAudio input channel and any current limits.

## Out of scope

- Firmware updates.
- Changes to the existing four-channel playback mapping.
- Unsupported sample rates or channel layouts inferred only from generic Ploytec code.
