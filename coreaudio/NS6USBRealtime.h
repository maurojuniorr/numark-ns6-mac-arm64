#ifndef NS6_USB_REALTIME_H
#define NS6_USB_REALTIME_H

#include <AudioToolbox/AudioWorkInterval.h>
#include <mach/mach_time.h>
#include <os/workgroup.h>
#include <os/object.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

typedef struct {
    os_workgroup_interval_t interval;
    os_workgroup_join_token_s token;
    bool joined;
    bool active;
} NS6USBRealtimeInterval;

static inline uint64_t ns6_usb_realtime_duration_ticks(uint64_t duration_ns,
                                                        uint32_t numer,
                                                        uint32_t denom) {
    if (numer == 0 || denom == 0) return 0;
    __uint128_t scaled = (__uint128_t)duration_ns * denom;
    __uint128_t ticks = (scaled + numer - 1) / numer;
    return ticks > UINT64_MAX ? UINT64_MAX : (uint64_t)ticks;
}

static inline uint64_t ns6_usb_realtime_deadline(uint64_t start,
                                                  uint64_t duration_ticks) {
    return duration_ticks > UINT64_MAX - start
        ? UINT64_MAX
        : start + duration_ticks;
}

static inline bool ns6_usb_realtime_interval_init(
    NS6USBRealtimeInterval *state) {
    if (!state) return false;
    memset(state, 0, sizeof(*state));
    state->interval = AudioWorkIntervalCreate(
        "Numark NS6 USB output", OS_CLOCK_MACH_ABSOLUTE_TIME, NULL);
    if (!state->interval) return false;
    int result = os_workgroup_join((os_workgroup_t)state->interval,
                                   &state->token);
    if (result != 0) {
        os_release(state->interval);
        state->interval = NULL;
        return false;
    }
    state->joined = true;
    return true;
}

static inline int ns6_usb_realtime_interval_begin(
    NS6USBRealtimeInterval *state, uint64_t start, uint64_t deadline) {
    if (!state || !state->interval || !state->joined || state->active ||
        deadline <= start) return -1;
    int result = os_workgroup_interval_start(state->interval, start, deadline,
                                              NULL);
    if (result == 0) state->active = true;
    return result;
}

static inline int ns6_usb_realtime_interval_finish(
    NS6USBRealtimeInterval *state) {
    if (!state || !state->interval || !state->joined || !state->active)
        return -1;
    int result = os_workgroup_interval_finish(state->interval, NULL);
    state->active = false;
    return result;
}

static inline void ns6_usb_realtime_interval_destroy(
    NS6USBRealtimeInterval *state) {
    if (!state) return;
    if (state->active) (void)ns6_usb_realtime_interval_finish(state);
    if (state->joined && state->interval)
        os_workgroup_leave((os_workgroup_t)state->interval, &state->token);
    if (state->interval) os_release(state->interval);
    memset(state, 0, sizeof(*state));
}

#endif
