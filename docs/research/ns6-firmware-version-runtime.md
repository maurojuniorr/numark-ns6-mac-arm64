# NS6 firmware query on hardware

On 2026-10-06, package 0.2.105 was installed on the user's Apple Silicon Mac with the NS6 connected. The HAL driver and Status app both reported version 0.2.105. The driver log recorded two successful vendor request `0xc0/0x56` transactions whose actual response length was **5 bytes**, although the request asked for 8:

```
Numark NS6 firmware query failed: status 0x00000000, returned 5 of 8 bytes
```

The 0.2.105 parser incorrectly required exactly 8 bytes, so no firmware version reached NS6 Status. Package 0.2.106 accepts a response of 3–8 bytes, validates the known version fields, and logs the returned bytes for hardware confirmation. The current capture from the original Mojave driver contains no endpoint-zero control requests, so it cannot supply those bytes.

The 0.2.106 package has been built and unit-tested but has not yet been installed. Until its raw response is observed, the mapping from the response's first byte to the Windows panel's `K1` label remains unconfirmed.
