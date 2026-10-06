#include "NS6ClockControl.h"

#include <string.h>

#define NS6_NOMINAL_RATE_MILLIHZ UINT64_C(44100000)
#define NS6_MIN_OUTPUT_RATE_MILLIHZ UINT64_C(44000000)
#define NS6_MAX_OUTPUT_RATE_MILLIHZ UINT64_C(44100300)
void ns6_clock_control_init(NS6ClockControl *control) {
    if (!control) return;
    memset(control, 0, sizeof(*control));
    control->rate_millihz = NS6_NOMINAL_RATE_MILLIHZ;
    control->target_frames_per_ms = 44;
}

void ns6_clock_control_reset_feedback(NS6ClockControl *control) {
    if (!control) return;
    control->feedback_sum = 0;
    control->feedback_count = 0;
    control->feedback_index = 0;
    memset(control->feedback_window, 0, sizeof(control->feedback_window));
    control->rate_millihz = NS6_NOMINAL_RATE_MILLIHZ;
    control->requested_total = 0;
    control->sent_total = 0;
    control->locked = false;
    control->target_frames_per_ms = 44;
    control->adaptive_fraction_millionths = 0;
}

void ns6_clock_control_observe(NS6ClockControl *control, uint8_t frames_per_ms) {
    if (!control || (frames_per_ms != 44 && frames_per_ms != 45)) return;

    control->target_frames_per_ms = frames_per_ms;
    control->requested_total += frames_per_ms;
    control->locked = true;

    if (control->feedback_count == NS6_FEEDBACK_WINDOW)
        control->feedback_sum -= control->feedback_window[control->feedback_index];
    else
        ++control->feedback_count;
    control->feedback_window[control->feedback_index] = frames_per_ms;
    control->feedback_sum += frames_per_ms;
    control->feedback_index = (control->feedback_index + 1) % NS6_FEEDBACK_WINDOW;

    /* Diagnostic only. Clock correction uses cumulative requested/sent debt. */
    if (control->feedback_count == NS6_FEEDBACK_WINDOW)
        control->rate_millihz = (control->feedback_sum * UINT64_C(1000000)) /
                                NS6_FEEDBACK_WINDOW;
}

void ns6_clock_control_next_millisecond(NS6ClockControl *control, unsigned packet_frames[8]) {
    if (!control || !packet_frames) return;

    unsigned frames_per_ms;
#if NS6_ADAPTIVE_CLOCK_ENABLED
    /* A USB OUT transfer is prepared in blocks, so one latest 44/45 feedback
       report can cover several milliseconds. Repeating that one value across
       the block biases the sent frame count. Spread the measured feedback
       rate fractionally across milliseconds instead. */
    uint64_t rate = ns6_clock_control_output_rate_millihz(control);
    frames_per_ms = (unsigned)(rate / UINT64_C(1000000));
    control->adaptive_fraction_millionths += rate % UINT64_C(1000000);
    if (control->adaptive_fraction_millionths >= UINT64_C(1000000)) {
        control->adaptive_fraction_millionths -= UINT64_C(1000000);
        ++frames_per_ms;
    }
    if (frames_per_ms < 40) frames_per_ms = 40;
    if (frames_per_ms > 48) frames_per_ms = 48;
#else
    control->fixed_fraction += 100;
    frames_per_ms = 44;
    if (control->fixed_fraction >= 1000) {
        control->fixed_fraction -= 1000;
        ++frames_per_ms;
    }
#endif

    unsigned base = frames_per_ms / 8;
    unsigned remainder = frames_per_ms % 8;
    unsigned accumulator = 0;
    for (unsigned packet = 0; packet < 8; ++packet) {
        accumulator += remainder;
        packet_frames[packet] = base;
        if (accumulator >= 8) {
            ++packet_frames[packet];
            accumulator -= 8;
        }
    }
    control->sent_total += frames_per_ms;
}

int64_t ns6_clock_control_debt(const NS6ClockControl *control) {
    if (!control) return 0;
    if (control->requested_total >= control->sent_total)
        return (int64_t)(control->requested_total - control->sent_total);
    return -(int64_t)(control->sent_total - control->requested_total);
}

uint64_t ns6_clock_control_requested_total(const NS6ClockControl *control) {
    return control ? control->requested_total : 0;
}

uint64_t ns6_clock_control_sent_total(const NS6ClockControl *control) {
    return control ? control->sent_total : 0;
}

uint64_t ns6_clock_control_rate_millihz(const NS6ClockControl *control) {
    return control ? control->rate_millihz : 0;
}

bool ns6_clock_control_is_locked(const NS6ClockControl *control) {
    return control && control->locked;
}

bool ns6_clock_control_is_adaptive_enabled(void) {
    return NS6_ADAPTIVE_CLOCK_ENABLED != 0;
}

uint64_t ns6_clock_control_output_rate_millihz(const NS6ClockControl *control) {
    uint64_t rate = NS6_NOMINAL_RATE_MILLIHZ;
#if NS6_ADAPTIVE_CLOCK_ENABLED
    if (control && control->locked && control->rate_millihz)
        rate = control->rate_millihz;
    if (rate < NS6_MIN_OUTPUT_RATE_MILLIHZ) rate = NS6_MIN_OUTPUT_RATE_MILLIHZ;
    if (rate > NS6_MAX_OUTPUT_RATE_MILLIHZ) rate = NS6_MAX_OUTPUT_RATE_MILLIHZ;
#else
    (void)control;
#endif
    return rate;
}

double ns6_clock_control_source_frames_per_output_frame(const NS6ClockControl *control,
                                                         uint32_t queued_frames) {
    uint64_t measured = ns6_clock_control_output_rate_millihz(control);
    double step = (double)NS6_NOMINAL_RATE_MILLIHZ / (double)measured;

    /* Keep the SPSC queue away from both starvation and overflow. The USB
       feedback establishes the base clock ratio; queue error applies a slow,
       bounded trim to absorb residual host/device drift and startup backlog. */
    const double target_frames = 4096.0;
    double trim = ((double)queued_frames - target_frames) * 0.000001;
    /* Allow bounded SRC correction to absorb the measured producer/device
       mismatch before the 16K-frame ring fills and CoreAudio blocks are lost. */
    if (trim > 0.005) trim = 0.005;
    if (trim < -0.005) trim = -0.005;
    step *= 1.0 + trim;
    if (step > 1.005) step = 1.005;
    if (step < 0.995) step = 0.995;
    return step;
}
