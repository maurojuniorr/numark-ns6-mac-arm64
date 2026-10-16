# NS6 MIDI protocol experiment: vendor OUT control byte

## Baseline and current problem

The active test branch is `codex/audio-on-demand-from-0.2.123`, not the later
persistent-device-service implementation in the main checkout. Installed
bundle version before this experiment: 0.2.127.

The fresh five-minute log (2026-10-16, approximately 07:02:38–07:07:37, UTC−4)
contains 144 throttled MIDI completion-error messages (102 NotResponding and
42 Aborted), one logged PipeStalled submission error, and 229 stall-clear
messages. These are logged counts, not unthrottled physical error totals.
Audio health remains at 36 underruns / 182 frames, with zero USB audio
transfer/packet/short-packet errors. All health snapshots have an empty PCM
queue. There is also one stale-feedback warning. This is not evidence that
MIDI errors caused additional audio underruns during this interval.

## Fresh Ghidra verification

Read-only decompilation of the original `NumarkNS6Audio` kext, SHA-256
`e8a83948a9f68cd0e26114430a052f5bc4debbf73a3a4773ef8e3f39ebe4bbbd`:

- `AJ::configurationDone`, 0x2f2c: relevant report format is 42 bytes;
  retained output control state at offset 0x13bc starts at 0xe0.
- `AJ::fireOutputInternal`, 0x3b6c: fills the report with 0xfd, adds queued
  MIDI payload, and puts the retained control state in the final byte.
  `Queue256` can update that state separately from MIDI payload.
- `AJ::inACR_NS7`, 0x36d8: excludes the receive trailer from MIDI parsing.
- `AJ::inAsyncCompletionRoutine`, 0x3d64: successful reads are replenished;
  failed completions retire the request without clearing/rearming that pipe
  in this callback. Blindly copying this would risk exhausting our read pool.

The earlier 2026-10-14 flow audit independently counted 14,353 successful
vendor MIDI OUT packets across three captures: all had final byte E0.
MIDI IN reports had final byte 00. That capture count is historical evidence,
not a new reprocessing of the captures in this pass.

## Single-variable test 0.2.128

Change only the physical MIDI OUT report control byte from 00 to E0. Apply it
to the shared queue (including initialization SysEx) and the driver's LED
blackout packets. The bridge's blackout commands pass through that same queue.
Do not change MIDI IN parsing, audio clock, transport lifetime, read count,
recovery policy, or application mappings.

This corrects a verified protocol difference. The firmware meaning of E0's
bits is unknown, so neither a keepalive function nor a blink/stall fix is
claimed before hardware validation.

## Verification and hardware procedure

The report test includes the actual NS6MIDI.c packet producer and intercepts
blackout writes through a mock USB interface. It failed against the original
00 trailer, then passed with E0. It checks payload/padding, full 41-byte
payload preservation, oversize rejection, and all blackout reports. Parser,
recovery-queue and lease tests also passed. Package build and its USB
work-interval regression check passed.

The package is prepared, not installed automatically. Install from the exact
`coreaudio/dist-midi-control-e0-0.2.128/` directory; the version number alone
must not be confused with older experiments from another development line.
The pre-existing 0.2.127 package remains in `coreaudio/dist/` for rollback.

After installation, power-cycle NS6 for a clean comparison, start one audio
source, then close it and leave the device idle for five minutes. Also verify
Mixxx MIDI input and LED output, including reopening and closing Mixxx. Compare
NotResponding, PipeStalled, pipe-clear counts and health counters with the
saved baseline. If failures remain, E0 alone is insufficient; do not add
clock changes or suppress errors to declare success.

Evidence, script, baseline log and artifact hashes are under
`../diagnostics/2026-10-16-midi-control-e0/`.


## Hardware follow-up: 08:28:34–08:58:34, 2026-10-16 (UTC−4)

Installed version 0.2.128 executable SHA256 matches the E0 build: `85b7d913eeaeccd9bd3b3149a7145ff1949ec10fe1aff57809cb0483bad3b3fa`. One audio helper PID (52228) throughout the interval. Disk verification alone does not establish the loaded binary identity.

- 501 throttled MIDI input completion failure log entries.
- 682 MIDI input pipe stall-clear entries.
- 4 MIDI input submission failures, all 0xe000404f (PipeStalled).
- 177 audio health records: zero underruns, USB transfer errors, packet errors and short packets.
- Audio queue remains empty; callback-gap records identify paused transport. No audio stream resume/idle transitions in this interval. This is an idle test, not an audible playback qualification.

Conclusion: E0 matches the vendor protocol, but does not eliminate the observed MIDI recovery/stall behavior. Baseline used a different observation interval and duration, so lower per-minute counts cannot establish a causal improvement. Investigate the initial read failure and the endpoint-wide effect of clear-stall on outstanding reads separately; do not attribute the fault to audio starvation from these data. Raw evidence: `docs/diagnostics/2026-10-16-midi-control-e0/hardware-followup-30m.log`.
