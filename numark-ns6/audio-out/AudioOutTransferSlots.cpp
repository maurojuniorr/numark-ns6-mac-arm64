#include "AudioOutTransferSlots.hpp"

AudioOutTransferSlots::AudioOutTransferSlots() noexcept {
    for (auto& slot : inUse_) slot.store(false, std::memory_order_relaxed);
}

std::uint32_t AudioOutTransferSlots::reserve(AudioOutputRing& ring,
                                             AudioPacketPolicy& policy) noexcept {
    if (!accepting_.load(std::memory_order_acquire)) return kNoSlot;
    for (std::uint32_t i = 0; i < kSlotCount; ++i) {
        bool free = false;
        if (!inUse_[i].compare_exchange_strong(free, true, std::memory_order_acq_rel)) continue;
        if (!accepting_.load(std::memory_order_acquire)) {
            inUse_[i].store(false, std::memory_order_release);
            return kNoSlot;
        }
        if (!packets_[i].build(ring, policy)) {
            inUse_[i].store(false, std::memory_order_release);
            return kNoSlot;
        }
        inFlight_.fetch_add(1, std::memory_order_release);
        return i;
    }
    return kNoSlot;
}

bool AudioOutTransferSlots::complete(std::uint32_t slot) noexcept {
    if (slot >= kSlotCount) return false;
    bool inUse = true;
    if (!inUse_[slot].compare_exchange_strong(inUse, false, std::memory_order_acq_rel)) return false;
    inFlight_.fetch_sub(1, std::memory_order_release);
    return true;
}

const AudioOutTransferBatch* AudioOutTransferSlots::packet(std::uint32_t slot) const noexcept {
    return slot < kSlotCount ? &packets_[slot] : nullptr;
}
