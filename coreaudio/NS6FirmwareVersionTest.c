#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

bool ns6_firmware_version_decode(const uint8_t response[8], size_t length, char output[32]);

int main(void) {
    char version[32] = {0};
    const uint8_t known_k1_version[8] = {1, 1, 3, 0, 0, 0, 0, 0};
    assert(ns6_firmware_version_decode(known_k1_version, sizeof(known_k1_version), version));
    assert(strcmp(version, "1.0.3 (K1)") == 0);
    assert(ns6_firmware_version_decode(known_k1_version, 5, version));
    assert(strcmp(version, "1.0.3 (K1)") == 0);

    const uint8_t unknown_variant[8] = {2, 1, 3, 0, 0, 0, 0, 0};
    assert(!ns6_firmware_version_decode(unknown_variant, sizeof(unknown_variant), version));
    assert(!ns6_firmware_version_decode(known_k1_version, 2, version));
    assert(!ns6_firmware_version_decode(NULL, 8, version));
    puts("NS6 firmware version decoder tests passed");
    return 0;
}
