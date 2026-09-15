#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

class BloomFilter {
public:
    static constexpr int HASH_COUNT = 4;

    // ~10 бит на ключ при четырёх хэшах даёт false positive rate порядка 1%.
    static size_t suggested_bytes(size_t expected_keys) {
        const size_t bytes = (expected_keys * 10 + 7) / 8;
        return bytes < 8 ? 8 : bytes;
    }

    explicit BloomFilter(size_t size_bytes) : bits_(size_bytes > 0 ? size_bytes : 1, 0) {}

    explicit BloomFilter(std::vector<uint8_t> data) : bits_(std::move(data)) {
        if (bits_.empty()) bits_.assign(1, 0);
    }

    void add(const std::string& key) {
        const uint32_t h1 = hash1(key);
        const uint32_t h2 = hash2(key);
        for (int i = 0; i < HASH_COUNT; ++i) {
            const size_t bit = (h1 + static_cast<uint32_t>(i) * h2) % size_bits();
            bits_[bit / 8] |= static_cast<uint8_t>(1u << (bit % 8));
        }
    }

    bool maybe_contains(const std::string& key) const {
        const uint32_t h1 = hash1(key);
        const uint32_t h2 = hash2(key);
        for (int i = 0; i < HASH_COUNT; ++i) {
            const size_t bit = (h1 + static_cast<uint32_t>(i) * h2) % size_bits();
            if ((bits_[bit / 8] & (1u << (bit % 8))) == 0) return false;
        }
        return true;
    }

    const std::vector<uint8_t>& data() const { return bits_; }

private:
    std::vector<uint8_t> bits_;

    size_t size_bits() const { return bits_.size() * 8; }

    // Символы приводятся к uint8_t: char знаковый не на всех платформах, а хэш
    // должен совпадать на записи и на чтении файла.
    static uint32_t hash1(const std::string& s) {
        uint32_t h = 0x811c9dc5;
        for (char c : s) h = (h ^ static_cast<uint8_t>(c)) * 0x01000193;
        return h;
    }

    static uint32_t hash2(const std::string& s) {
        uint32_t h = 0;
        for (char c : s) h = h * 31u + static_cast<uint8_t>(c);
        return h | 1u; // нечётный шаг не даёт всем хэшам схлопнуться в одну позицию
    }
};
