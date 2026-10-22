#include "AudioDeviceLifecycle.hpp"

AudioIOTransition AudioDeviceLifecycle::startIO(std::uint32_t clientID) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    const bool wasIdle = clients_.activeStarts() == 0;
    if (!clients_.start(clientID)) return AudioIOTransition::Rejected;
    return wasIdle ? AudioIOTransition::FirstClientStarted
                   : AudioIOTransition::AdditionalClientStarted;
}

AudioIOTransition AudioDeviceLifecycle::stopIO(std::uint32_t clientID) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!clients_.stop(clientID)) return AudioIOTransition::ClientNotActive;
    return clients_.activeStarts() == 0 ? AudioIOTransition::LastClientStopped
                                        : AudioIOTransition::ClientStoppedStillActive;
}

AudioIOTransition AudioDeviceLifecycle::removeClient(std::uint32_t clientID) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!clients_.removeClient(clientID)) return AudioIOTransition::ClientNotActive;
    return clients_.activeStarts() == 0 ? AudioIOTransition::LastClientStopped
                                        : AudioIOTransition::ClientStoppedStillActive;
}

std::uint64_t AudioDeviceLifecycle::activeStarts() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return clients_.activeStarts();
}
