# USB latency activity experiment — 0.2.131

Question approved for testing: does retaining a documented ProcessInfo latency activity during the real USB worker lifetime reduce the large idle callback gaps preceding MIDI NotResponding?

Baseline .130: idle begins09:19:13.985, callback gap113924us at09:19:14.354, first MIDI error09:19:14.355. 101 thresholded gap records during paused transport, none during active in this capture. System releases the client's idle-sleep assertion at idle. This is correlation, not proof of helper App Nap.

Implementation: NS6USBActivity.m owns a retained ProcessInfo token using UserInitiatedAllowingIdleSystemSleep | LatencyCritical. Worker obtains token before initialization and ends it on its sole exit after cleanup, including initialization failure. Token remains during silence/recovery while the worker exists. No fake CoreAudio client, explicit system/display sleep prevention, or MIDI/clock/queue changes. Option scope is process-wide, so other work in the HAL helper can also be affected. A non-null token is not proof of realtime scheduling.

All NS6USB.c build callers (driver and USB test) link Foundation and the Objective-C implementation. The activity smoke test exercises overlapping token acquisitions/releases and null release. Report, parser, recovery and lease tests pass; production package build, USB scheduling import regression check and git diff --check pass. Hardware untested.

Package: coreaudio/dist-usb-activity-0.2.131/NumarkNS6-0.2.131.pkg
Package SHA256: 368f8387ecdac8941ef47b1d4775e27923df2de9e08852be647791de01630cd7
Driver SHA256: 6abfd884a2ead62e1ee6a86b76b489532b57ca7ad81efcb32f70c26e3d49724d
Rollback: coreaudio/dist-midi-read-age-0.2.130/NumarkNS6-0.2.130.pkg

Capture all Numark NS6 logs before first playback, play then pause15s, close/reopen app, move MIDI controls. Compare pre-recovery gaps, success count, initial failures, stall count and audio health with .130. Verify latency activity begin: token available1 in actual HAL helper. Stop after a few minutes to inspect. Monitor energy consumption; remove activity experiment if ineffective rather than layering more scheduling changes.

## Initial hardware capture

Helper64808 acquired activity token at09:36:53.425; first audio start09:36:53.434. Live capture ends09:37:31.355; retrieved unified follow-up extends to09:37:53.881. No MIDI errors, pipe clears or callback-gap messages; six unified audio-health samples report zero underruns and USB errors. However there is NO audio-idle transition. This validates acquisition in the host and short active playback only; idle hypothesis remains untested. Next capture must include last-client StopIO/idle, at least30s idle and reopening. Pausing a deck can leave CoreAudio active and is not equivalent to last-client closure.

## Idle result: experiment failed to resolve symptom

Second capture09:38:58–09:40:56, same helper64808, no live-drop notices: three idle entries, two audio resumes. 54 input clears, 186 thresholded USB callback gaps (all paused1), median32808us, max1162433us. Audio health reaches18 underrun events/94 frames, max burst6, with zero USB transfer/packet/short-packet errors. Do not infer the token caused this increase; workloads and transitions differ.

First idle09:39:00.962; USB gap53456us09:39:01.268 with frame newest age56509us; first MIDI NotResponding09:39:01.279, before any clear. That read was pending91933ms, last successful completion91642ms earlier. Therefore a fixed16-second read timeout is inconsistent with these two runs. Activity token does not prevent the idle callback delay or MIDI failure; no evidence to retain it as a production fix.

submit() passes frameStart0 to LowLatencyWriteIsochPipeAsync, no advancing explicit frame target in this branch. Therefore hypothesis of our explicit deadline counter continuing into the past is unsupported. Large completion timestamp age indicates late callback processing but does not localize delay between kernel, event delivery and thread scheduling. Next discriminating evidence is a short privileged process sample/trace during idle rather than another speculative package. `/usr/bin/sample 64808 3` with sandbox escalation failed because process inspection requires administrator privileges (not a sandbox auto-review rejection). No installed driver changes made.

## Process sample, 14:23:17 (same helper64808)

4092 observations of the USB worker: 3472 (~84.8%) in CFRunLoopServiceMachPort/mach_msg receive, 471 (~11.5%) in IOConnectCallAsyncMethod under USB submit. These are stack sampling proportions, not CPU utilization or individual wait durations. Worker also executes audio/feedback callbacks and MIDI error logging. MIDI writer has3593 observations in condition wait and484 in nanosleep (recovery backoff); five are under clear-pipe path. No dominant mutex-blocked stack or busy PCM loop observed. Waiting for async events is normal and does not itself prove delayed delivery or App Nap. The sample cannot resolve whether missing time is controller delivery versus scheduler delay; requires event timeline or active-state comparative sample. Live-log reflection/XPC is visible in callback paths, so diagnostic logging can perturb timing and should be controlled in comparisons. No fix established. Correlated unified-log retrieval was interrupted before execution; no corresponding window file exists.
