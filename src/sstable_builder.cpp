#include "sstable.hpp"

#include "bloom_filter.hpp"
#include "encoding.hpp"

#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <filesystem>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

void write_all(int fd, const uint8_t* data, size_t size, const std::string& what) {
    size_t done = 0;
    while (done < size) {
        const ssize_t n = ::write(fd, data + done, size - done);
        if (n < 0) {
            if (errno == EINTR) continue;
            throw std::system_error(errno, std::generic_category(), "запись " + what);
        }
        done += static_cast<size_t>(n);
    }
}

void fsync_directory(const std::string& file_path) {
    const std::string dir = std::filesystem::path(file_path).parent_path().string();
    const int fd = ::open(dir.empty() ? "." : dir.c_str(), O_RDONLY | O_DIRECTORY);
    if (fd < 0) throw std::system_error(errno, std::generic_category(), "открытие каталога " + dir);
    const int rc = ::fsync(fd);
    const int saved = errno;
    ::close(fd);
    if (rc != 0) throw std::system_error(saved, std::generic_category(), "fsync каталога " + dir);
}

// Видимость файла должна быть атомарной: сначала полностью записанный временный
// файл, затем fsync, затем rename. Иначе краш оставит битый .sst в списке таблиц.
void write_file_atomically(const std::string& path, const std::vector<uint8_t>& buffer) {
    const std::string tmp = path + ".tmp";

    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) throw std::system_error(errno, std::generic_category(), "создание " + tmp);

    try {
        write_all(fd, buffer.data(), buffer.size(), tmp);
        if (::fsync(fd) != 0) {
            throw std::system_error(errno, std::generic_category(), "fsync " + tmp);
        }
    } catch (...) {
        ::close(fd);
        ::unlink(tmp.c_str());
        throw;
    }
    ::close(fd);

    if (::rename(tmp.c_str(), path.c_str()) != 0) {
        const int saved = errno;
        ::unlink(tmp.c_str());
        throw std::system_error(saved, std::generic_category(), "rename " + tmp);
    }
    fsync_directory(path);
}

} // namespace

SSTableBuilder::SSTableBuilder(std::string filename) : filename_(std::move(filename)) {}

void SSTableBuilder::build(const std::map<std::string, Record>& data) {
    using namespace encoding;

    std::vector<uint8_t> buffer;
    buffer.insert(buffer.end(), sst_format::MAGIC, sst_format::MAGIC + sst_format::MAGIC_SIZE);

    BloomFilter filter(BloomFilter::suggested_bytes(data.size()));
    std::vector<std::pair<const std::string*, uint64_t>> index;
    index.reserve(data.size());

    for (const auto& [key, record] : data) {
        index.emplace_back(&key, static_cast<uint64_t>(buffer.size()));
        filter.add(key);

        put_u32(buffer, static_cast<uint32_t>(key.size()));
        buffer.insert(buffer.end(), key.begin(), key.end());
        buffer.push_back(record.tombstone ? 1 : 0);
        put_u32(buffer, static_cast<uint32_t>(record.value.size()));
        buffer.insert(buffer.end(), record.value.begin(), record.value.end());
    }

    const uint64_t bloom_offset = buffer.size();
    buffer.insert(buffer.end(), filter.data().begin(), filter.data().end());

    const uint64_t index_offset = buffer.size();
    for (const auto& [key, offset] : index) {
        put_u32(buffer, static_cast<uint32_t>(key->size()));
        buffer.insert(buffer.end(), key->begin(), key->end());
        put_u64(buffer, offset);
    }

    put_u64(buffer, index_offset);
    put_u64(buffer, static_cast<uint64_t>(index.size()));
    put_u64(buffer, bloom_offset);
    put_u64(buffer, static_cast<uint64_t>(filter.data().size()));
    buffer.insert(buffer.end(), sst_format::MAGIC, sst_format::MAGIC + sst_format::MAGIC_SIZE);

    write_file_atomically(filename_, buffer);
}
