#ifndef NS6_CLOCK_CONTROL_H
#define NS6_CLOCK_CONTROL_H

#include <stdbool.h>
#include <stdint.h>

#ifndef NS6_ADAPTIVE_CLOCK_ENABLED
#define NS6_ADAPTIVE_CLOCK_ENABLED 0
#endif
#ifndef NS6_FEEDBACK_PATTERN_SCHEDULER_ENABLED
#define NS6_FEEDBACK_PATTERN_SCHEDULER_ENABLED 0
#endif

#define NS6_FEEDBACK_WINDOW 40000u
#define NS6_FEEDBACK_PATTERN_QUEUE_CAPACITY 256u

typedef struct {
    uint64_t feedback_sum;
    uint64_t rate_millihz;
    uint64_t requested_total;
    uint64_t sent_total;
    uint64_t fixed_fraction;
    uint64_t adaptive_fraction_millionths;
    uint32_t feedback_count;
    uint32_t feedback_index;
    bool locked;
    uint8_t target_frames_per_ms;
    uint8_t feedback_window[NS6_FEEDBACK_WINDOW];
    uint8_t pattern_queue[NS6_FEEDBACK_PATTERN_QUEUE_CAPACITY];
    uint16_t pattern_queue_read;
    uint16_t pattern_queue_count;
} NS6ClockControl;

void ns6_clock_control_init(NS6ClockControl *control);
void ns6_clock_control_reset_feedback(NS6ClockControl *control);
void ns6_clock_control_observe(NS6ClockControl *control, uint8_t frames_per_ms);
void ns6_clock_control_next_millisecond(NS6ClockControl *control, unsigned packet_frames[8]);
int64_t ns6_clock_control_debt(const NS6ClockControl *control);
uint64_t ns6_clock_control_requested_total(const NS6ClockControl *control);
uint64_t ns6_clock_control_sent_total(const NS6ClockControl *control);
uint64_t ns6_clock_control_rate_millihz(const NS6ClockControl *control);
bool ns6_clock_control_is_locked(const NS6ClockControl *control);
bool ns6_clock_control_is_adaptive_enabled(void);
bool ns6_clock_control_is_feedback_pattern_enabled(void);
uint64_t ns6_clock_control_output_rate_millihz(const NS6ClockControl *control);
double ns6_clock_control_source_frames_per_output_frame(const NS6ClockControl *control,
                                                         uint32_t queued_frames);

#endif
