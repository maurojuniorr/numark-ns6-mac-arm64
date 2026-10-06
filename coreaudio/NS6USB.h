#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Runs outside CoreAudio's real-time thread. */
bool ns6_usb_start(void);
bool ns6_usb_request_restart(void);
bool ns6_usb_is_ready(void);
void ns6_usb_stop(void);
void ns6_usb_set_paused(bool paused);
void ns6_usb_set_startup_buffer_frames(uint32_t frames);
bool ns6_usb_get_firmware_version(char output[32]);
void ns6_usb_submit_pcm(const uint8_t *pcm24, uint32_t frames);
