#ifndef NS6_AUDIO_KEEPALIVE_POLICY_HPP
#define NS6_AUDIO_KEEPALIVE_POLICY_HPP

#include <cstdint>

namespace ns6 {

enum class AudioKeepaliveAction { None, Start, Stop };

constexpr AudioKeepaliveAction audioKeepaliveAction(std::uint32_t activeDevice,
                                                    std::uint32_t discoveredDevice) noexcept {
    if (activeDevice != 0 && activeDevice != discoveredDevice) return AudioKeepaliveAction::Stop;
    if (activeDevice == 0 && discoveredDevice != 0) return AudioKeepaliveAction::Start;
    return AudioKeepaliveAction::None;
}

}  // namespace ns6

#endif
