# EP 0x83 recovery batch experiment (0.2.135)

With 0.2.134 and 16 pending reads, five minutes of logs showed 26 `kIOReturnNotResponding` completions, 32 pipe clears and 360 `kIOReturnAborted` completions. No audio queue underruns were logged. A pipe clear aborts other pending reads, so an immediate resubmit can race with those callbacks and trigger another clear.

0.2.135 leaves audio and the 16-read depth unchanged. It tracks which read slots are still in flight. After a pipe clear, the MIDI worker waits up to 250 ms for the aborted callbacks, combines their recovery requests and resubmits completed slots once. The wait is on the MIDI writer thread, not the CoreAudio render callback. The cap prevents an indefinitely blocked callback from holding MIDI writes forever.

Compare `NotResponding`, `Aborted`, pipe-clear counts, LED behavior and MIDI control response over at least five minutes. A reduced abort-to-clear multiplier would support the batch-recovery hypothesis. The original `NotResponding` trigger remains under investigation.
