#include "memtable.hpp"

void MemTable::put(const std::string& key, const std::string& value, bool tombstone) {
    std::unique_lock lock(mutex_);

    auto [it, inserted] = table_.try_emplace(key, Record{value, tombstone});
    if (inserted) {
        size_bytes_ += key.size() + value.size();
    } else {
        // При перезаписи старое значение уходит из памяти, иначе счётчик уползает
        // вверх и flush срабатывает раньше реального заполнения.
        size_bytes_ -= it->second.value.size();
        size_bytes_ += value.size();
        it->second.value = value;
        it->second.tombstone = tombstone;
    }
}

std::optional<Record> MemTable::get(const std::string& key) const {
    std::shared_lock lock(mutex_);
    const auto it = table_.find(key);
    if (it == table_.end()) return std::nullopt;
    return it->second;
}

std::map<std::string, Record> MemTable::snapshot() const {
    std::shared_lock lock(mutex_);
    return table_;
}

size_t MemTable::approximate_size() const {
    std::shared_lock lock(mutex_);
    return size_bytes_;
}

bool MemTable::empty() const {
    std::shared_lock lock(mutex_);
    return table_.empty();
}

void MemTable::clear() {
    std::unique_lock lock(mutex_);
    table_.clear();
    size_bytes_ = 0;
}
