# NS6 audio stability checkpoint — 0.2.14

## Current listening test

- The maintainer reports zero occurrences so far of the intermittent temporary
  distortion while testing 0.2.14; the listening period is still ongoing.
- A recent five-minute Unified Log read showed continuous output activity.
- Across 299 output health reports, the measured clock ranged from 44,099.975 to
  44,100.350 Hz and the output queue ranged from 4,066 to 4,818 frames; the
  source step stayed close to 1.0.
- Output reported zero ring-underrun frames and zero transfer failures.
- Feedback reported zero short packets, zero transfer errors, and zero skipped
  reads. The cumulative out-of-range count was about 0.13% of packets; this is
  being rejected by the parser and did not coincide with output errors in this
  snapshot.

## Investigation note

The intermittent distortion may be related to USB feedback acquisition or its
isochronous scheduling. Version 0.2.14 changed feedback reads to automatic
scheduling (`startFrame = 0`), matching the legacy reference, and recent logs
show no short or skipped feedback reads. This is a working hypothesis, not a
confirmed cause: the distortion was absent during this snapshot, so there is no
event to correlate with feedback or output counters yet. If it recurs, record
the exact time and compare the surrounding feedback, queue, clock, and output
health reports before changing the implementation.

## Follow-up since 16:55

- Through 18:15, 4,803 output reports still showed zero ring-underrun frames and
  zero transfer failures. Queue ranged from 4,059 to 4,896 frames; measured
  clock ranged from 44,099.900 to 44,100.450 Hz.
- Across 6,412,000 feedback packets, the cumulative short-packet count rose by
  four at 17:53 and stayed at four through 18:15. Length errors, transfer
  errors, and skipped reads remained zero. The four short packets are about
  0.00006% of all packets; they have not coincided with output errors in these
  logs.

## Follow-up since 18:15

- Through 20:39, 8,613 output health reports showed zero ring-underrun frames
  and zero transfer failures. Queue ranged from 4,059 to 5,613 frames; measured
  clock ranged from 44,099.800 to 44,100.425 Hz.
- Across 15,004,000 feedback packets, short packets increased from four to six
  at 18:53 and remained at six through 20:39. Length errors, transfer errors,
  and skipped reads remained zero. The six short packets are about 0.00004% of
  all packets and have no coincident output underruns in the logs.
