#pragma once

#include <cstdint>
#include <vector>

// Явный little-endian: файлы, записанные на одной машине, должны читаться на любой другой.
namespace encoding {

inline void put_u32(std::vector<uint8_t>& out, uint32_t value) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(value >> (8 * i)));
}

inline void put_u64(std::vector<uint8_t>& out, uint64_t value) {
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<uint8_t>(value >> (8 * i)));
}

inline uint32_t get_u32(const uint8_t* src) {
    uint32_t value = 0;
    for (int i = 0; i < 4; ++i) value |= static_cast<uint32_t>(src[i]) << (8 * i);
    return value;
}

inline uint64_t get_u64(const uint8_t* src) {
    uint64_t value = 0;
    for (int i = 0; i < 8; ++i) value |= static_cast<uint64_t>(src[i]) << (8 * i);
    return value;
}

} // namespace encoding
