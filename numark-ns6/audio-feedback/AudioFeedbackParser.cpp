#include "AudioFeedbackParser.hpp"

#include <algorithm>

AudioFeedbackObservation AudioFeedbackParser::parse(bool transferOk, bool frameOk,
                                                    const std::uint8_t* bytes,
                                                    std::size_t byteCount) noexcept {
    AudioFeedbackObservation result;
    result.byteCount = static_cast<std::uint16_t>(
            std::min(byteCount, static_cast<std::size_t>(UINT16_MAX)));
    if (bytes != nullptr) {
        std::copy_n(bytes, std::min(byteCount, kReportBytes), result.raw.begin());
    }
    if (!transferOk || !frameOk || bytes == nullptr) {
        result.kind = AudioFeedbackKind::transferError;
    } else if (byteCount < kReportBytes) {
        result.kind = AudioFeedbackKind::shortPacket;
    } else if (byteCount != kReportBytes) {
        result.kind = AudioFeedbackKind::unexpectedLength;
    } else if (bytes[0] < kMinimumFramesPerMillisecond ||
               bytes[0] > kMaximumFramesPerMillisecond) {
        result.kind = AudioFeedbackKind::outOfRange;
    } else {
        result.kind = AudioFeedbackKind::valid;
        result.framesPerMillisecond = bytes[0];
    }
    return result;
}
