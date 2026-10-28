# EP 0x83 explicit timeout experiment (0.2.136)

0.2.135 kept 16 reads and batched recovery but still logged a `kIOReturnNotResponding` after about nine seconds without MIDI data; the next submission returned `kIOUSBPipeStalled`, followed by a clear and aborted reads. The original-driver capture had a successful EP 0x83 read after a gap of more than one minute.

0.2.136 changes only the USB read API from `ReadPipeAsync` to `ReadPipeAsyncTO` with both no-data and completion timeouts explicitly zero. The MIDI recovery batching and audio path are unchanged. This isolates whether an implicit host timeout triggers the first failure. If `NotResponding` persists, the cause is elsewhere and this API change should not be treated as a fix.

Observe at least ten minutes with audio active and paused, MIDI controls responsive, and compare `NotResponding`, pipe clears, aborts and USB LED oscillations to 0.2.135.
