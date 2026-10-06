# NS6 audio-capture transport evidence

## Summary

The original Ploytec kext includes generic capture/audio-input code, but the available NS6-specific USB capture does not show a dedicated PCM input endpoint. Do not add a CoreAudio input stream from these findings alone.

## Hopper observations

- `PGConfigurationDescriptor::findAudioStreamingInInterface` and `PGInterfaceDescriptor::isAudioStreamingIn` are generic Ploytec descriptor routines.
- `PGDevice::bulkAudioRun` calls `PGDevice::setAJDMAInputChannels`; its pseudocode configures the input channel count and prepares bulk audio IRPs. `PGDevice::resumeStreaming` invokes `bulkAudioRun` together with `rtsIsocAudioRun` for some selected settings.
- These routines prove that the Ploytec framework contains input-capable code. They do not by themselves prove that the NS6 routes microphone/line audio through that path.

## NS6 USB capture observations

Artifact: `/Users/maurojunior/Downloads/ns6_full_10min.pcapng`, USB Darwin capture from macOS 10.14.6 on XHC20. TShark identified NS6 device address 13. Full-capture endpoint/type counts:

| Endpoint | Transfer | Direction | Captured transfers |
| --- | --- | --- | ---: |
| `0x02` | isochronous | host → NS6 | 300,056 |
| `0x81` | isochronous | NS6 → host | 300,056 |
| `0x86` | bulk | NS6 → host | 330,826 |
| `0x83` | bulk | NS6 → host | 4,270 |
| `0x04` | bulk | host → NS6 | 19,472 |

Endpoint `0x81` returned 12 bytes per captured transfer, with `2c2c2c` (44-frame feedback values) followed by placeholder `aaaaaa` values. This is feedback, not a PCM capture stream. Endpoint `0x83`/`0x04` carry MIDI. Endpoint `0x02` is playback. The remaining IN endpoint `0x86` is already identified by the project protocol notes as the waveform-display bulk endpoint; its captured 10,240-byte payload is highly structured and consists largely of `0x00`/`0x01` values rather than 24-bit PCM samples.

## Descriptors from the connected NS6

On 2026-10-06, the read-only `tools/ns6-probe` queried the physical NS6 (`15e4:0079`). Its single 80-byte configuration has only these alternate settings:

| Interface | Alternate | Endpoint | Transfer | Max packet |
| --- | ---: | --- | --- | ---: |
| 0 | 0 | none | — | — |
| 0 | 1 | `0x02` | isochronous OUT | 156 |
| 0 | 1 | `0x83` | bulk IN | 512 |
| 0 | 1 | `0x04` | bulk OUT | 512 |
| 1 | 0 | none | — | — |
| 1 | 1 | `0x81` | isochronous IN | 64 |
| 1 | 1 | `0x86` | bulk IN | 512 |

No alternate setting exposes another input endpoint. Any host capture would have to use `0x81` or `0x86` in a mode that was not identified in the available trace. The observed payloads identify `0x81` as clock feedback and `0x86` as structured waveform data during that session.

## Stop/go result

The capture does not contain a separate NS6 audio-input endpoint or recognizable PCM input payload. The Ploytec input functions are generic, and there is no NS6-specific descriptor/payload evidence tying them to the physical inputs. Therefore Task 1's stop/go gate is **not met**: do not publish an input device or treat `0x81`/`0x86` as capture.

To resolve the mismatch with the Windows panel's “Inputs: 2”, a useful next artifact would be a USB capture from the original Windows driver while recording known audio fed into the NS6's physical input. Compare IN payloads with the input silent and with a 440 Hz signal. That would show whether the Windows driver activates another alternate setting or repurposes an existing endpoint.

## Limits

- The pcap shows one captured configuration/session; it does not exhaust every possible alternate setting.
- The descriptors rule out an undiscovered alternate-setting endpoint on this hardware, but cannot rule out a different payload mode on an existing endpoint.
- The Windows panel's input count establishes software-reported channels, not the USB transport or that the NS6 sends those physical inputs to the host.
