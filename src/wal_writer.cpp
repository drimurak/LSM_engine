#include "wal_writer.hpp"

#include "encoding.hpp"

#include <cerrno>
#include <fcntl.h>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>
#include <zlib.h>

WalWriter::WalWriter(std::string filename) : filename_(std::move(filename)) {
    // O_DIRECT здесь был бы вредом, а не пользой: он обходит page cache, но ничего
    // не гарантирует про попадание на носитель и требует выравнивания записи по
    // размеру сектора. Долговечность даёт fdatasync, а не обход кэша.
    fd_ = ::open(filename_.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd_ < 0) {
        throw std::system_error(errno, std::generic_category(), "открытие WAL " + filename_);
    }
}

WalWriter::~WalWriter() {
    if (fd_ >= 0) ::close(fd_);
}

void WalWriter::append(const std::string& key, const std::string& value, bool tombstone) {
    std::vector<uint8_t> record;
    record.reserve(HEADER_SIZE + key.size() + value.size());

    record.resize(4, 0); // место под CRC, считается по остальной части записи
    encoding::put_u32(record, static_cast<uint32_t>(key.size()));
    encoding::put_u32(record, static_cast<uint32_t>(value.size()));
    record.push_back(tombstone ? 1 : 0);
    record.insert(record.end(), key.begin(), key.end());
    record.insert(record.end(), value.begin(), value.end());

    const uint32_t checksum =
        static_cast<uint32_t>(::crc32(0L, record.data() + 4, static_cast<uInt>(record.size() - 4)));
    for (int i = 0; i < 4; ++i) record[i] = static_cast<uint8_t>(checksum >> (8 * i));

    size_t done = 0;
    while (done < record.size()) {
        const ssize_t n = ::write(fd_, record.data() + done, record.size() - done);
        if (n < 0) {
            if (errno == EINTR) continue;
            throw std::system_error(errno, std::generic_category(), "запись в WAL " + filename_);
        }
        done += static_cast<size_t>(n);
    }

    if (::fdatasync(fd_) != 0) {
        throw std::system_error(errno, std::generic_category(), "fdatasync WAL " + filename_);
    }
}

void WalWriter::reset() {
    if (::ftruncate(fd_, 0) != 0) {
        throw std::system_error(errno, std::generic_category(), "усечение WAL " + filename_);
    }
    if (::fdatasync(fd_) != 0) {
        throw std::system_error(errno, std::generic_category(), "fdatasync WAL " + filename_);
    }
}
