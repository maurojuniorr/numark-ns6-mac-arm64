#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include "NS6CaptureRing.h"

/* Runs outside CoreAudio's real-time thread. */
bool ns6_usb_start(void);
bool ns6_usb_request_restart(void);
bool ns6_usb_is_ready(void);
void ns6_usb_stop(void);
void ns6_usb_set_paused(bool paused);
void ns6_usb_reset_capture_session(void);
void ns6_usb_log_io_transition(const char *event, uint32_t client,
    unsigned clients_before, unsigned clients_after);
size_t ns6_usb_capture_read(float *output,size_t frames);
void ns6_usb_bind_shared_capture(NS6CaptureRing *ring, atomic_bool *requested,
                                atomic_uint_fast64_t *last_demand_ns);
void ns6_usb_set_startup_buffer_frames(uint32_t frames);
bool ns6_usb_get_firmware_version(char output[32]);
void ns6_usb_submit_pcm(const uint8_t *pcm24, uint32_t frames);
size_t ns6_usb_capture_read(float *output, size_t frames);
