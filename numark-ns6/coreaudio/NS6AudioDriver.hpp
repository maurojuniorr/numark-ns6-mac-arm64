#pragma once

#include <CoreAudio/AudioServerPlugIn.h>

class AudioOutputEngine;
class AudioInputEngine;

namespace NS6AudioDriver {
constexpr AudioObjectID kDeviceObjectID = 2;
constexpr AudioObjectID kOutputStreamObjectID = 3;
constexpr AudioObjectID kInputStreamObjectID = 4;
constexpr UInt32 kSampleRate = 44100;
constexpr UInt32 kChannels = 4;
constexpr UInt32 kInputChannels = 2;
constexpr UInt32 kBytesPerFrame = kChannels * sizeof(Float32);

OSStatus processOutputMix(AudioOutputEngine& engine, UInt32 operation, UInt32 frames,
                          const Float32* samples) noexcept;
OSStatus processInput(AudioInputEngine& engine, UInt32 operation, UInt32 frames,
                      Float32* samples) noexcept;
}
