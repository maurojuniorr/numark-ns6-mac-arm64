#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool ns6_firmware_version_decode(const uint8_t response[8], size_t length, char output[32]);
