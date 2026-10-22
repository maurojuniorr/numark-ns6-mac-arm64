#pragma once

#include <array>
#include <cstdint>

class AudioClientLifecycle final {
public:
    bool start(std::uint32_t clientID) noexcept;
    bool stop(std::uint32_t clientID) noexcept;
    bool removeClient(std::uint32_t clientID) noexcept;
    std::uint64_t activeStarts() const noexcept { return activeStarts_; }

private:
    struct Client final {
        std::uint32_t id = 0;
        std::uint32_t starts = 0;
    };

    static constexpr std::size_t kClientCapacity = 64;
    std::array<Client, kClientCapacity> clients_{};
    std::uint64_t activeStarts_ = 0;
};
