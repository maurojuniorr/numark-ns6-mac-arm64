#include "MIDIInputEventEncoder.hpp"

std::size_t MIDIInputEventEncoder::encode(const std::uint8_t* bytes, std::size_t length,
                                          std::uint32_t* words,
                                          std::size_t capacity) noexcept {
    if (bytes == nullptr || words == nullptr) return 0;
    std::size_t consumed = 0;
    std::size_t count = 0;
    while (consumed < length) {
        const std::uint8_t status = bytes[consumed];
        if (status < 0x80 || status > 0xef) return 0;
        const std::size_t size = status >= 0xc0 && status <= 0xdf ? 2 : 3;
        if (length - consumed < size || count == capacity) return 0;
        const std::uint8_t data1 = bytes[consumed + 1];
        const std::uint8_t data2 = size == 3 ? bytes[consumed + 2] : 0;
        if ((data1 & 0x80) != 0 || (data2 & 0x80) != 0) return 0;
        words[count++] = (0x2u << 28) | (static_cast<std::uint32_t>(status) << 16) |
                         (static_cast<std::uint32_t>(data1) << 8) | data2;
        consumed += size;
    }
    return count;
}
