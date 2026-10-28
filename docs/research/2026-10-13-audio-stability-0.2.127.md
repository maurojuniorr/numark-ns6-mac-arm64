# NS6 audio stability checkpoint — 0.2.127

## Test

- Hardware: original Numark NS6, M1 Pro Mac, macOS Sequoia.
- Observed log window: 2026-10-13 10:45:11–12:15:09 local time (90 minutes).
- The maintainer reported no audible clicks, pops, or glitches during playback.
- Driver package reported active: `io.github.maurojuniorr.numark-ns6`, version
  `0.2.127`.

## Measurements

- 5,264 adaptive-clock reports; output-rate range: 44,099.975–44,100.175 Hz.
- 538 audio-health reports; output USB transfer errors: 0; isochronous packet
  errors: 0.
- Underrun counter reached 12 events / 62 frames (maximum burst: 6 frames),
  then remained at that value through the end of the observation window.
- Feedback short-packet, out-of-range, and read-failure events were clustered
  around 11:03 while the queue was empty and the stream was marked paused. The
  helper process changed around this time. The logs do not establish the cause
  of that process transition or prove that these feedback events occurred while
  audible playback was active.
- Feedback read-error counter reached 5 and remained there; no output transfer
  or packet errors followed.

## Assessment

This is a successful long playback test for audible clock-related glitches:
the maintainer heard none, the clock remained close to 44.1 kHz, and output
transfer/packet error counters stayed at zero. The isolated feedback recovery
episode and the 12-frame underrun tally remain recorded as diagnostic findings,
but did not correspond to an audible artifact in this session. This checkpoint
does not establish behavior on other hardware, macOS releases, or audio clients.
