#pragma once

#include "AudioClientLifecycle.hpp"

#include <cstdint>
#include <mutex>

enum class AudioIOTransition : std::uint8_t {
    Rejected,
    FirstClientStarted,
    AdditionalClientStarted,
    ClientStoppedStillActive,
    LastClientStopped,
    ClientNotActive
};

// Each CoreAudio device tracks its own clients; the driver shares one USB session.
class AudioDeviceLifecycle final {
public:
    AudioIOTransition startIO(std::uint32_t clientID) noexcept;
    AudioIOTransition stopIO(std::uint32_t clientID) noexcept;
    AudioIOTransition removeClient(std::uint32_t clientID) noexcept;
    std::uint64_t activeStarts() const noexcept;

private:
    mutable std::mutex mutex_;
    AudioClientLifecycle clients_;
};
