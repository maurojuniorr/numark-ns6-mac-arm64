# NS6 firmware query on hardware

On 2026-10-06, package 0.2.105 was installed on the user's Apple Silicon Mac with the NS6 connected. The HAL driver and Status app both reported version 0.2.105. The driver log recorded two successful vendor request `0xc0/0x56` transactions whose actual response length was **5 bytes**, although the request asked for 8:

```
Numark NS6 firmware query failed: status 0x00000000, returned 5 of 8 bytes
```

The 0.2.105 parser incorrectly required exactly 8 bytes, so no firmware version reached NS6 Status. Package 0.2.106 accepts a response of 3–8 bytes, validates the known version fields, and logs the returned bytes for hardware confirmation. The current capture from the original Mojave driver contains no endpoint-zero control requests, so it cannot supply those bytes.

After installing 0.2.106, two starts returned the same five bytes:

```
31 01 03 02 02
```

The first byte is ASCII `1` (`0x31`), not numeric `0x01` as the initial decoder assumed. The next bytes match major `1` and decimal minor/patch `03`, so the observed response agrees with the Windows panel's `1.0.3 (K1)`. Version 0.2.107 accepts this measured response. Other device variants remain unverified and are rejected rather than guessed.

Version 0.2.107 was installed on 2026-10-06. The driver log reported `Numark NS6 firmware version response: 1.0.3 (K1)`, and the installed NS6 Status window visibly showed `Firmware Version 1.0.3 (K1)` while Opera held an active audio flow.
