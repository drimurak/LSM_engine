#pragma once

#include "memtable.hpp"
#include "sstable_reader.hpp"
#include "wal_writer.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <thread>
#include <vector>

class StorageEngine {
public:
    explicit StorageEngine(const std::string& base_path);
    ~StorageEngine();

    StorageEngine(const StorageEngine&) = delete;
    StorageEngine& operator=(const StorageEngine&) = delete;

    void put(const std::string& key, const std::string& value);
    void remove(const std::string& key);
    std::optional<std::string> get(const std::string& key) const;

    // Сливает все SSTable в один и физически удаляет tombstone'ы.
    void compact();
    void flush();

    size_t sstable_count() const;
    size_t memtable_bytes() const;

private:
    struct SSTable {
        uint64_t seq = 0;
        std::string path;
        std::shared_ptr<SSTableReader> reader;
    };

    static constexpr size_t FLUSH_THRESHOLD = 4ull * 1024 * 1024;
    static constexpr size_t COMPACTION_TRIGGER = 5;

    std::string path_;
    std::unique_ptr<WalWriter> wal_;
    std::unique_ptr<MemTable> memtable_;
    std::vector<SSTable> sstables_; // от старых к новым
    uint64_t next_seq_ = 1;

    // Защищает memtable_, wal_ и sstables_ целиком: и для писателей, и для читателей,
    // и для фоновой компакции.
    mutable std::shared_mutex mutex_;
    std::mutex compaction_mutex_;
    std::atomic<bool> compacting_{false};
    std::thread compaction_thread_;

    std::string sst_path(uint64_t seq) const;
    void load_sstables();
    void recover_from_wal();
    void write(const std::string& key, const std::string& value, bool tombstone);
    void flush_locked();
    void maybe_start_compaction_locked();
};
