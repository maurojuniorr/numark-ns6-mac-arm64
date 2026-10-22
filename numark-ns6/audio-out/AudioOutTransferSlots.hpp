#pragma once

#include "AudioOutTransferBatch.hpp"

#include <array>
#include <atomic>
#include <cstdint>

class AudioOutTransferSlots final {
public:
    static constexpr std::uint32_t kSlotCount = 8;
    static constexpr std::uint32_t kNoSlot = UINT32_MAX;
    AudioOutTransferSlots() noexcept;

    // A reserved packet remains valid until complete() releases its slot.
    std::uint32_t reserve(AudioOutputRing& ring, AudioPacketPolicy& policy) noexcept;
    bool complete(std::uint32_t slot) noexcept;
    void stopAccepting() noexcept { accepting_.store(false, std::memory_order_release); }
    std::uint32_t inFlightCount() const noexcept { return inFlight_.load(std::memory_order_acquire); }
    const AudioOutTransferBatch* packet(std::uint32_t slot) const noexcept;

private:
    std::array<AudioOutTransferBatch, kSlotCount> packets_{};
    std::array<std::atomic<bool>, kSlotCount> inUse_{};
    std::atomic<bool> accepting_{true};
    std::atomic<std::uint32_t> inFlight_{0};
};
