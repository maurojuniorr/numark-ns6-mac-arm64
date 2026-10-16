# MIDI recovery trace, test 0.2.129

E0 test 0.2.128 retained repeated input failures and pipe clears during idle. Do not count reduced logged messages as successful recovery.

Apple IOUSBLib documentation says clearing the pipe returns pending operations and invokes their asynchronous callbacks. Thus one clear can affect all four reads. Source: https://github.com/apple-oss-distributions/IOUSBFamily/blob/main/IOUSBFamily/Headers/IOUSBLib.h . Modern IOUSBHost clearStall documentation also states pending requests abort.

The existing Ghidra export at `docs/diagnostics/2026-10-16-midi-control-e0/ghidra-midi-protocol.c`, AJ::inAsyncCompletionRoutine (0x3d64), replenishes a successful read, but releases a failed request without clearing the pipe in that routine. This does not establish what lower layers do.

Current recovery consumes a per-slot bitmask, optionally clears the entire endpoint, then resubmits slots. Other callbacks can arrive during this sequence. A single global error-log throttle previously hid their status and slot. Existing logs therefore cannot prove that recovery caused the initial NotResponding event.

Diagnostic change only: per-slot/per-error-class logging throttles, cumulative error-class totals, input-clear epoch and clear-in-progress snapshot. Epoch increments before ClearPipeStallBothEnds; callbacks may arrive after it returns, so active=false alone does not exclude recovery effects. No change to read scheduling, recovery, audio clock, queue, or client lifecycle. Counters last for the helper process lifetime.

Validation: report, parser, recovery and lease tests pass; package builds; git diff --check clean. Hardware validation pending. Increased error logging is bounded per slot and class, but may add overhead in a failure storm; use for short captures.

Package: coreaudio/dist-midi-recovery-trace-0.2.129/NumarkNS6-0.2.129.pkg
SHA256: e426696d87b3c48374f5d3edcda94aadb52f45c938179cb9f520116f28d96beb

Test: start log capture before starting the first audio source; play, pause, wait 15 seconds, close and reopen the app. Capture about two minutes. Identify NotResponding before the first input-clear epoch, then Aborted/PipeStalled callbacks at later epochs. Do not disable all recovery based solely on vendor callback behavior: doing so could drain every pending read.

## Hardware result: 09:04–09:07, October 16

Helper PID60441. Live capture lost messages; retrieved unified history for 09:04:30–09:07:15 and preserved both logs under docs/diagnostics/2026-10-16-midi-recovery-trace. Historical and live timestamps differ slightly; do not combine them for sub-millisecond ordering. Throttling also prevents counting lines as total callbacks.

First failure is slot0 NotResponding at clear epoch0, active0. Immediate resubmission returns PipeStalled, still epoch0. The first clear then returns slots1/2/3 with Aborted, epoch1, active1. This establishes that this initial failure preceded recovery; the three aborted reads are recovery side effects. Subsequent cycles repeat the same slot pattern. Live final cumulative counters are 52 NotResponding, 52 PipeStalled, 156 Aborted and input clear epoch52 (three aborts per clear). These are process cumulative totals, not necessarily all events inside the captured window.

Audio health goes from zero to 3 underrun events /14 frames, max burst5; USB audio transfer and packet errors remain zero. Therefore do not claim this test is free of underruns. Causation between MIDI clears and audio underruns is not established.

Next investigation: initial EP83 NotResponding, including read age, successful completion rate, actual read length/packet boundaries, and host controller state. Merely suppressing Aborted logs or removing recovery will not solve the initiating failure and could leave MIDI dead. No recovery behavior changed following this capture.

## Diagnostic 0.2.130: age of the initiating read

Reviewed the historical grouped abort/drain/clear implementation and its .152 results: it did not prevent initiating failures, so it is not reintroduced as a root fix. System-log search around 09:04:42–46 provided no explanatory USB host-controller error.

Added atomic submission timestamp per read, last successful callback timestamp and cumulative successful callback count. Existing throttled error message now reports read age and last-success age. Success means successful USB completion, not necessarily a decoded MIDI event. Ages of zero with no success count mean no successful callback yet; submission failures have near-zero read age by definition. Counts persist for the helper lifetime. No recovery or audio behavior changes.

Report, parser, recovery and lease tests pass; package build and diff whitespace check pass. Hardware pending. Package: coreaudio/dist-midi-read-age-0.2.130/NumarkNS6-0.2.130.pkg; SHA256 54970d8562795e9e59e190abc6f3611513cfc0ef53834bc95cce01e27fcce322.

Capture before starting playback, pause 15 seconds, manipulate jog/knobs briefly, then close/reopen audio application. Compare read age and successful callback counts before the first failure and subsequent recovery cycles. Keep capture short to limit diagnostic logging load.

## 0.2.130 hardware observation

Session PID62654 starts 09:18:50.575; handshake reply observed 09:18:51.376. Audio idle transition at 09:19:13.985 precedes the first MIDI NotResponding near 09:19:14.3. Live first-error metrics: read age16244ms, last success15939ms, successful completion count33. Count remains33 through the capture ending09:19:44.620, when last-success age reaches46235ms and clear epoch33. Subsequent failed read ages can be under100ms, so this is not evidence of a fixed16-second request timeout. No successful USB input completions occurred between these logged failures; this alone cannot distinguish lack of control traffic from failure to deliver user input. User control movement timing not captured. Audio health remains zero underruns/USB errors during this interval.

This run strengthens the correlation with the transition to idle: substantial MIDI quiet time already existed before idle, but the initial fault follows idle entry. Inspect paused-output scheduling and host power/lifecycle changes next. It does not prove silence samples or power management caused the failure. Live and historical timestamps differ slightly; use each source internally.

## Idle scheduling comparison

In the 09:18:45–09:19:50 unified capture, zero USB callback-gap messages occur with paused0; 101 occur with paused1, median logged gap32482us and max288632us. These are thresholded diagnostics, not the full callback distribution. A gap113924us at09:19:14.354 precedes the first MIDI error at09:19:14.355. That record reports previous submit65us and fill3us wall/2us CPU, successful transfer status and no bad frames. Successful transfer status does not prove continuous bus coverage. Submitted queue is8x32 microframes=32ms nominal horizon. Callback latency over32ms threatens replenishment; actual bus gaps still require scheduling/trace evidence.

CoreAudio logs IOWorkLoopDeinit at09:19:13.986 and releases PreventUserIdleSystemSleep assertion at09:19:13.988 on behalf of Mixxx62704. Helper is62654. This is NOT proof the helper entered App Nap or that the power assertion controls its scheduling. Apple's App Nap documentation describes CPU/timer/I/O throttling; ProcessInfo activity API permits latency-critical work. These are hypotheses to test, not established causes.

Code inspection: USB worker QoS is set once, not explicitly reduced at idle; OUT packet frame scheduling still runs before the paused branch. Idle sends zero PCM and calls transport_discard, whose initialized flag avoids repeated full-ring clearing. HAL also calls discard at StopIO; potential concurrent reset remains a separate concern. No USB close/abort is issued directly by this idle branch.

Next bounded experiment: hold a documented ProcessInfo activity token for the lifetime of the real USB transport, release at worker exit, without keeping a fake audio client open or altering MIDI recovery/clock. Measure first pre-recovery gap, maximum callback gap, MIDI successful completions and faults with/without token. UserInitiatedAllowingIdleSystemSleep plus LatencyCritical avoids deliberately preventing system idle sleep, but can increase energy use. Need confirm API behavior in HAL host and test before treating as fix. Do not promise hard realtime scheduling from an activity token.

References: https://developer.apple.com/library/archive/documentation/Performance/Conceptual/power_efficiency_guidelines_osx/AppNap.html ; https://developer.apple.com/documentation/foundation/processinfo/beginactivity(options:reason:) .
