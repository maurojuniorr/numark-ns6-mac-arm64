#include "NS6FirmwareVersion.h"

#include <stdio.h>

bool ns6_firmware_version_decode(const uint8_t response[8], size_t length, char output[32]) {
    if (output) output[0] = '\0';
    if (!response || !output || length < 3 || length > 8 || response[0] != 0x31 || response[1] == 0 || response[2] > 99)
        return false;

    /* Ploytec's scanFirmwareVersion stores response[1] as the major version
       and splits response[2] as decimal minor/patch digits. The available
       Numark panel identifies the observed ASCII '1' (0x31) as K1. */
    unsigned major = response[1];
    unsigned minor = response[2] / 10;
    unsigned patch = response[2] % 10;
    int written = snprintf(output, 32, "%u.%u.%u (K1)", major, minor, patch);
    if (written < 0 || written >= 32) {
        output[0] = '\0';
        return false;
    }
    return true;
}
