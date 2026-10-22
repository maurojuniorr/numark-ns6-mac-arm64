#include "AudioClientLifecycle.hpp"

#include <limits>

bool AudioClientLifecycle::start(std::uint32_t clientID) noexcept {
    Client* freeSlot = nullptr;
    for (auto& client : clients_) {
        if (client.starts == 0) {
            if (freeSlot == nullptr) freeSlot = &client;
            continue;
        }
        if (client.id == clientID) {
            if (client.starts == std::numeric_limits<std::uint32_t>::max()) return false;
            ++client.starts;
            ++activeStarts_;
            return true;
        }
    }
    if (freeSlot == nullptr) return false;
    freeSlot->id = clientID;
    freeSlot->starts = 1;
    ++activeStarts_;
    return true;
}

bool AudioClientLifecycle::stop(std::uint32_t clientID) noexcept {
    for (auto& client : clients_) {
        if (client.starts == 0 || client.id != clientID) continue;
        --client.starts;
        --activeStarts_;
        if (client.starts == 0) client.id = 0;
        return true;
    }
    return false;
}

bool AudioClientLifecycle::removeClient(std::uint32_t clientID) noexcept {
    for (auto& client : clients_) {
        if (client.starts == 0 || client.id != clientID) continue;
        activeStarts_ -= client.starts;
        client = {};
        return true;
    }
    return false;
}
