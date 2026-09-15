#include "sstable_reader.hpp"

#include "encoding.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>
#include <utility>

namespace {

[[noreturn]] void corrupt(const std::string& file, const std::string& reason) {
    throw std::runtime_error("Повреждённый SSTable " + file + ": " + reason);
}

} // namespace

SSTableReader::SSTableReader(std::string filename) : filename_(std::move(filename)) {
    fd_ = ::open(filename_.c_str(), O_RDONLY);
    if (fd_ < 0) {
        throw std::system_error(errno, std::generic_category(), "открытие " + filename_);
    }

    try {
        struct stat st {};
        if (::fstat(fd_, &st) != 0) {
            throw std::system_error(errno, std::generic_category(), "fstat " + filename_);
        }
        file_size_ = static_cast<uint64_t>(st.st_size);

        if (file_size_ < sst_format::MAGIC_SIZE + sst_format::FOOTER_SIZE) {
            corrupt(filename_, "файл короче заголовка и футера");
        }

        char head[sst_format::MAGIC_SIZE];
        read_exact(head, sizeof(head), 0);
        if (std::memcmp(head, sst_format::MAGIC, sst_format::MAGIC_SIZE) != 0) {
            corrupt(filename_, "неизвестная сигнатура формата");
        }

        uint8_t footer[sst_format::FOOTER_SIZE];
        const uint64_t footer_offset = file_size_ - sst_format::FOOTER_SIZE;
        read_exact(footer, sizeof(footer), footer_offset);
        if (std::memcmp(footer + 4 * sizeof(uint64_t), sst_format::MAGIC, sst_format::MAGIC_SIZE) != 0) {
            corrupt(filename_, "футер не завершён сигнатурой (файл обрезан)");
        }

        const uint64_t index_offset = encoding::get_u64(footer);
        const uint64_t index_count = encoding::get_u64(footer + 8);
        const uint64_t bloom_offset = encoding::get_u64(footer + 16);
        const uint64_t bloom_bytes = encoding::get_u64(footer + 24);

        // Секции обязаны идти строго подряд и помещаться в файл — иначе любое
        // смещение из футера способно увести чтение за пределы данных.
        if (bloom_offset < sst_format::MAGIC_SIZE || bloom_offset > footer_offset ||
            bloom_bytes > footer_offset - bloom_offset ||
            bloom_offset + bloom_bytes != index_offset || index_offset > footer_offset) {
            corrupt(filename_, "несогласованные смещения секций");
        }

        data_end_ = bloom_offset;

        std::vector<uint8_t> bloom(static_cast<size_t>(bloom_bytes));
        if (bloom_bytes > 0) read_exact(bloom.data(), bloom.size(), bloom_offset);
        filter_ = BloomFilter(std::move(bloom));

        const size_t index_bytes = static_cast<size_t>(footer_offset - index_offset);
        std::vector<uint8_t> raw(index_bytes);
        if (index_bytes > 0) read_exact(raw.data(), raw.size(), index_offset);

        index_.reserve(static_cast<size_t>(index_count));
        size_t cursor = 0;
        for (uint64_t i = 0; i < index_count; ++i) {
            if (cursor + 4 > index_bytes) corrupt(filename_, "индекс обрезан");
            const uint32_t k_size = encoding::get_u32(raw.data() + cursor);
            cursor += 4;

            if (k_size == 0 || k_size > sst_format::MAX_KEY_SIZE || cursor + k_size + 8 > index_bytes) {
                corrupt(filename_, "некорректная запись индекса");
            }
            std::string key(reinterpret_cast<const char*>(raw.data() + cursor), k_size);
            cursor += k_size;

            const uint64_t offset = encoding::get_u64(raw.data() + cursor);
            cursor += 8;

            if (offset < sst_format::MAGIC_SIZE || offset >= data_end_) {
                corrupt(filename_, "смещение записи вне секции данных");
            }
            if (!index_.empty() && !(index_.back().key < key)) {
                corrupt(filename_, "индекс не отсортирован");
            }
            index_.push_back({std::move(key), offset});
        }
    } catch (...) {
        ::close(fd_);
        fd_ = -1;
        throw;
    }
}

SSTableReader::~SSTableReader() {
    if (fd_ >= 0) ::close(fd_);
}

void SSTableReader::read_exact(void* dst, size_t size, uint64_t offset) const {
    auto* out = static_cast<uint8_t*>(dst);
    size_t done = 0;
    while (done < size) {
        const ssize_t n = ::pread(fd_, out + done, size - done, static_cast<off_t>(offset + done));
        if (n < 0) {
            if (errno == EINTR) continue;
            throw std::system_error(errno, std::generic_category(), "чтение " + filename_);
        }
        if (n == 0) corrupt(filename_, "неожиданный конец файла");
        done += static_cast<size_t>(n);
    }
}

Record SSTableReader::read_record(uint64_t offset, const std::string& expected_key) const {
    if (offset + 4 > data_end_) corrupt(filename_, "запись выходит за секцию данных");

    uint8_t header[4];
    read_exact(header, sizeof(header), offset);
    const uint32_t k_size = encoding::get_u32(header);
    if (k_size != expected_key.size() || offset + 4 + k_size + 5 > data_end_) {
        corrupt(filename_, "длина ключа не совпадает с индексом");
    }

    std::string key(k_size, '\0');
    read_exact(key.data(), k_size, offset + 4);
    if (key != expected_key) corrupt(filename_, "ключ не совпадает с индексом");

    uint8_t meta[5];
    read_exact(meta, sizeof(meta), offset + 4 + k_size);
    const bool tombstone = meta[0] != 0;
    const uint32_t v_size = encoding::get_u32(meta + 1);

    const uint64_t value_offset = offset + 4 + k_size + 5;
    if (v_size > sst_format::MAX_VALUE_SIZE || value_offset + v_size > data_end_) {
        corrupt(filename_, "длина значения выходит за секцию данных");
    }

    Record record;
    record.tombstone = tombstone;
    record.value.resize(v_size);
    if (v_size > 0) read_exact(record.value.data(), v_size, value_offset);
    return record;
}

std::optional<Record> SSTableReader::get(const std::string& key) const {
    if (!filter_.maybe_contains(key)) return std::nullopt;

    // Индекс отсортирован по ключу, поэтому поиск бинарный, а не линейный проход.
    const auto it = std::lower_bound(index_.begin(), index_.end(), key,
                                     [](const IndexEntry& entry, const std::string& k) {
                                         return entry.key < k;
                                     });
    if (it == index_.end() || it->key != key) return std::nullopt;

    return read_record(it->offset, it->key);
}

std::map<std::string, Record> SSTableReader::load_all() const {
    std::map<std::string, Record> result;
    for (const auto& entry : index_) {
        result.emplace_hint(result.end(), entry.key, read_record(entry.offset, entry.key));
    }
    return result;
}
