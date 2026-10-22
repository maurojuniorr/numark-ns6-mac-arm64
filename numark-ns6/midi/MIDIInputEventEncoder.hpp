#pragma once

#include <cstddef>
#include <cstdint>

class MIDIInputEventEncoder final {
public:
    static std::size_t encode(const std::uint8_t* bytes, std::size_t length,
                              std::uint32_t* words, std::size_t capacity) noexcept;
};
