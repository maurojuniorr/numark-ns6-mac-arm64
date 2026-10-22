#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

enum class AudioFeedbackKind : std::uint8_t {
    valid,
    shortPacket,
    unexpectedLength,
    outOfRange,
    transferError,
};

struct AudioFeedbackObservation final {
    AudioFeedbackKind kind = AudioFeedbackKind::transferError;
    std::array<std::uint8_t, 3> raw{};
    std::uint16_t byteCount = 0;
    std::uint8_t framesPerMillisecond = 0;
};

class AudioFeedbackParser final {
public:
    static constexpr std::uint8_t kMinimumFramesPerMillisecond = 42;
    static constexpr std::uint8_t kMaximumFramesPerMillisecond = 46;
    static constexpr std::size_t kReportBytes = 3;

    static AudioFeedbackObservation parse(bool transferOk, bool frameOk,
                                          const std::uint8_t* bytes,
                                          std::size_t byteCount) noexcept;
};
