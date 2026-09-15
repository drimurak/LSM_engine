#pragma once

#include "bloom_filter.hpp"
#include "sstable.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

// Индекс и фильтр Блума загружаются в память один раз при открытии, данные читаются
// через pread по удерживаемому дескриптору. Дескриптор держит inode живым, поэтому
// удаление файла компакцией не может выдернуть его из-под параллельного чтения.
class SSTableReader {
public:
    explicit SSTableReader(std::string filename);
    ~SSTableReader();

    SSTableReader(const SSTableReader&) = delete;
    SSTableReader& operator=(const SSTableReader&) = delete;

    std::optional<Record> get(const std::string& key) const;
    std::map<std::string, Record> load_all() const;

    const std::string& path() const { return filename_; }
    size_t key_count() const { return index_.size(); }

private:
    struct IndexEntry {
        std::string key;
        uint64_t offset;
    };

    std::string filename_;
    int fd_ = -1;
    uint64_t file_size_ = 0;
    uint64_t data_end_ = 0;
    std::vector<IndexEntry> index_;
    BloomFilter filter_{std::vector<uint8_t>{}};

    void read_exact(void* dst, size_t size, uint64_t offset) const;
    Record read_record(uint64_t offset, const std::string& expected_key) const;
};
