#pragma once

#include "sstable.hpp"

#include <map>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>

class MemTable {
public:
    void put(const std::string& key, const std::string& value, bool tombstone);
    std::optional<Record> get(const std::string& key) const;

    std::map<std::string, Record> snapshot() const;

    size_t approximate_size() const;
    bool empty() const;
    void clear();

private:
    std::map<std::string, Record> table_;
    size_t size_bytes_ = 0;
    mutable std::shared_mutex mutex_;
};
