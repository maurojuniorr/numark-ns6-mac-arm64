# Numark NS6 on macOS

Created and maintained by **Mauro Junior** ([maurojuniorr](https://github.com/maurojuniorr)).

This is the arm64 macOS implementation. The original Numark HAL bundles
installed on some Macs are Intel-only and cannot load on Apple Silicon.

The Core Audio HAL owns the NS6 USB interfaces, runs the vendor initialization
sequence, and publishes four playback channels at 44.1 kHz S24_3LE.

## MIDI

The HAL owns the USB session and sends MIDI over a local loopback socket to
the companion **Numark NS6 MIDI Bridge**. The bridge runs in the logged-in
user session and publishes two CoreMIDI endpoints:

- **Numark NS6** input receives the controller's packed 42-byte USB MIDI
  messages through endpoint `0x83`.
- **Numark NS6** output accepts Note/CC messages from DJ software and sends
  them to endpoint `0x04` for the controller LEDs.

Only the HAL claims the NS6 USB interface; the bridge never opens it. This
keeps MIDI and audio in one USB session while ensuring the endpoints are
visible to Mixxx and other desktop applications.

The installer registers the bridge as a LaunchAgent, so it starts with macOS.
After installing, select **Numark NS6** in Mixxx. Its input and output ports
share that name so Mixxx pairs them automatically.

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
