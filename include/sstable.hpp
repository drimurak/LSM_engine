#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>

// Значение вместе с признаком удаления. Tombstone — отдельный флаг, а не магическая
// строка: иначе пользователь, записавший эту строку как значение, терял бы ключ.
struct Record {
    std::string value;
    bool tombstone = false;
};

namespace sst_format {

// Магия содержит версию формата, чтобы файлы старых версий отвергались явно,
// а не интерпретировались как мусор.
inline constexpr char MAGIC[] = {'L', 'S', 'M', 'S', 'S', 'T', 0x01, 0x00};
inline constexpr size_t MAGIC_SIZE = sizeof(MAGIC);

// index_offset, index_count, bloom_offset, bloom_bytes + магия в конце файла.
inline constexpr size_t FOOTER_SIZE = 4 * sizeof(uint64_t) + MAGIC_SIZE;

inline constexpr uint32_t MAX_KEY_SIZE = 64u * 1024;
inline constexpr uint32_t MAX_VALUE_SIZE = 64u * 1024 * 1024;

} // namespace sst_format

class SSTableBuilder {
public:
    explicit SSTableBuilder(std::string filename);

    // Пишет файл во временный путь, fsync-ает и атомарно переименовывает в целевой,
    // поэтому частично записанный SSTable никогда не виден читателям.
    void build(const std::map<std::string, Record>& data);

private:
    std::string filename_;
};
