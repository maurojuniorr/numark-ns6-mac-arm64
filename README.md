# Numark NS6 on macOS

Created and maintained by **Mauro Junior** ([maurojuniorr](https://github.com/maurojuniorr)).

This is the arm64 macOS implementation. The original Numark HAL bundles
installed on some Macs are Intel-only and cannot load on Apple Silicon.

The Core Audio HAL owns the NS6 USB interfaces, runs the vendor initialization
sequence, and publishes four playback channels at 44.1 kHz S24_3LE.

## MIDI

When an app starts playback, the HAL also publishes two CoreMIDI endpoints:

- **Numark NS6 Controls** receives the controller's packed 42-byte USB MIDI
  messages through endpoint `0x83`.
- **Numark NS6 LEDs** accepts Note/CC messages from DJ software and sends them
  to endpoint `0x04` for the controller LEDs.

Audio and MIDI deliberately share the same USB session. This prevents the
second client race that occurs when a standalone MIDI bridge tries to claim the
NS6 while Core Audio is streaming.

## Hardware probe

Install the build dependencies once:

```sh
brew install libusb pkgconf
```

Then build and run the read-only descriptor probe:

```sh
cd macos/tools
make ns6-libusb-probe
./ns6-libusb-probe
```

On managed development environments the command needs to run outside their
sandbox. On a normal Terminal session it only enumerates the device and reads
its descriptors; it neither claims an interface nor sends USB traffic.

The expected NS6 descriptor layout is:

| Interface | Alternate setting | Endpoint | Purpose |
| --- | --- | --- | --- |
| 0 | 1 | `0x02` | ISO OUT, four-channel S24_3LE playback |
| 0 | 1 | `0x83` / `0x04` | Bulk MIDI input / output |
| 1 | 1 | `0x81` | ISO IN feedback, currently known to be unreliable |
| 1 | 1 | `0x86` | Bulk waveform input, drained continuously |

## Clock work

The next transport milestone starts silence URBs, records actual completion
cadence and buffer occupancy, and writes a timestamped CSV trace. That gives
us a measured device rate before implementing the adaptive resampler.
