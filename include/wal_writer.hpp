#pragma once

#include <cstdint>
#include <string>

class WalWriter {
public:
    // [CRC32][K_SIZE][V_SIZE][FLAGS][KEY][VALUE]
    static constexpr size_t HEADER_SIZE = 13;

    explicit WalWriter(std::string filename);
    ~WalWriter();

    WalWriter(const WalWriter&) = delete;
    WalWriter& operator=(const WalWriter&) = delete;

    // Возвращает управление только после того, как запись оказалась на носителе.
    void append(const std::string& key, const std::string& value, bool tombstone);

    // Усечение после того, как memtable гарантированно записана в SSTable.
    void reset();

private:
    std::string filename_;
    int fd_ = -1;
};
