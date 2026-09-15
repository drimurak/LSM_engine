#include "engine.hpp"

#include "encoding.hpp"

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <zlib.h>

namespace {

constexpr const char* WAL_NAME = "/wal.log";
constexpr const char* SST_PREFIX = "sst_";
constexpr const char* SST_SUFFIX = ".sst";
constexpr int SEQ_DIGITS = 20;

std::optional<uint64_t> parse_seq(const std::string& filename) {
    const std::string prefix = SST_PREFIX;
    const std::string suffix = SST_SUFFIX;
    if (filename.size() != prefix.size() + SEQ_DIGITS + suffix.size()) return std::nullopt;
    if (filename.compare(0, prefix.size(), prefix) != 0) return std::nullopt;
    if (filename.compare(filename.size() - suffix.size(), suffix.size(), suffix) != 0) return std::nullopt;

    const char* begin = filename.data() + prefix.size();
    uint64_t seq = 0;
    const auto result = std::from_chars(begin, begin + SEQ_DIGITS, seq);
    if (result.ec != std::errc{} || result.ptr != begin + SEQ_DIGITS) return std::nullopt;
    return seq;
}

} // namespace

StorageEngine::StorageEngine(const std::string& base_path) : path_(base_path) {
    std::filesystem::create_directories(path_);

    memtable_ = std::make_unique<MemTable>();
    load_sstables();
    recover_from_wal();
    wal_ = std::make_unique<WalWriter>(path_ + WAL_NAME);
}

StorageEngine::~StorageEngine() {
    if (compaction_thread_.joinable()) compaction_thread_.join();
}

std::string StorageEngine::sst_path(uint64_t seq) const {
    std::ostringstream name;
    name << path_ << '/' << SST_PREFIX << std::setw(SEQ_DIGITS) << std::setfill('0') << seq << SST_SUFFIX;
    return name.str();
}

void StorageEngine::load_sstables() {
    std::vector<uint64_t> sequences;

    for (const auto& entry : std::filesystem::directory_iterator(path_)) {
        if (!entry.is_regular_file()) continue;
        const std::string name = entry.path().filename().string();

        // Временные файлы остаются только после краша посреди записи SSTable:
        // целевой файл в этом случае так и не появился, остаток бесполезен.
        if (entry.path().extension() == ".tmp") {
            std::filesystem::remove(entry.path());
            continue;
        }
        if (const auto seq = parse_seq(name)) sequences.push_back(*seq);
    }

    std::sort(sequences.begin(), sequences.end());
    for (const uint64_t seq : sequences) {
        const std::string path = sst_path(seq);
        sstables_.push_back({seq, path, std::make_shared<SSTableReader>(path)});
    }
    next_seq_ = sequences.empty() ? 1 : sequences.back() + 1;
}

void StorageEngine::recover_from_wal() {
    const std::string wal_path = path_ + WAL_NAME;
    std::ifstream in(wal_path, std::ios::binary);
    if (!in) return;

    std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (data.empty()) return;

    size_t cursor = 0;
    size_t recovered = 0;

    while (cursor + WalWriter::HEADER_SIZE <= data.size()) {
        const uint32_t stored_crc = encoding::get_u32(data.data() + cursor);
        const uint32_t k_size = encoding::get_u32(data.data() + cursor + 4);
        const uint32_t v_size = encoding::get_u32(data.data() + cursor + 8);
        const bool tombstone = data[cursor + 12] != 0;

        if (k_size == 0 || k_size > sst_format::MAX_KEY_SIZE || v_size > sst_format::MAX_VALUE_SIZE) break;

        const size_t payload = static_cast<size_t>(k_size) + v_size;
        if (cursor + WalWriter::HEADER_SIZE + payload > data.size()) break;

        // CRC считается по всей записи после самого поля контрольной суммы.
        const uint32_t actual_crc = static_cast<uint32_t>(
            ::crc32(0L, data.data() + cursor + 4, static_cast<uInt>(WalWriter::HEADER_SIZE - 4 + payload)));
        if (actual_crc != stored_crc) break;

        const char* key_ptr = reinterpret_cast<const char*>(data.data() + cursor + WalWriter::HEADER_SIZE);
        std::string key(key_ptr, k_size);
        std::string value(key_ptr + k_size, v_size);

        memtable_->put(key, value, tombstone);
        cursor += WalWriter::HEADER_SIZE + payload;
        ++recovered;
    }

    if (recovered > 0) {
        std::cout << "[Engine] Восстановлено записей из WAL: " << recovered << std::endl;
    }
    // Хвост после cursor — это оборванная последним крашем запись. Она не подтверждена
    // вызывающей стороне и отбрасывается при следующем усечении WAL.
    if (cursor < data.size()) {
        std::cout << "[Engine] Отброшен неполный хвост WAL: " << (data.size() - cursor) << " байт"
                  << std::endl;
    }
}

void StorageEngine::write(const std::string& key, const std::string& value, bool tombstone) {
    if (key.empty() || key.size() > sst_format::MAX_KEY_SIZE) {
        throw std::invalid_argument("Некорректная длина ключа");
    }
    if (value.size() > sst_format::MAX_VALUE_SIZE) {
        throw std::invalid_argument("Значение превышает допустимый размер");
    }

    std::unique_lock lock(mutex_);

    // Сначала WAL (с fdatasync), затем память: обратный порядок терял бы
    // подтверждённые записи при крахе.
    wal_->append(key, value, tombstone);
    memtable_->put(key, value, tombstone);

    if (memtable_->approximate_size() >= FLUSH_THRESHOLD) {
        flush_locked();
        maybe_start_compaction_locked();
    }
}

void StorageEngine::put(const std::string& key, const std::string& value) {
    write(key, value, false);
}

void StorageEngine::remove(const std::string& key) {
    write(key, std::string{}, true);
}

std::optional<std::string> StorageEngine::get(const std::string& key) const {
    std::shared_lock lock(mutex_);

    if (const auto record = memtable_->get(key)) {
        if (record->tombstone) return std::nullopt;
        return record->value;
    }

    // От новых файлов к старым: первое попадание и есть актуальная версия ключа.
    for (auto it = sstables_.rbegin(); it != sstables_.rend(); ++it) {
        if (const auto record = it->reader->get(key)) {
            if (record->tombstone) return std::nullopt;
            return record->value;
        }
    }

    return std::nullopt;
}

void StorageEngine::flush() {
    std::unique_lock lock(mutex_);
    flush_locked();
    maybe_start_compaction_locked();
}

void StorageEngine::flush_locked() {
    if (memtable_->empty()) return;

    const uint64_t seq = next_seq_;
    const std::string path = sst_path(seq);

    SSTableBuilder(path).build(memtable_->snapshot());
    auto reader = std::make_shared<SSTableReader>(path);

    // Память освобождается и WAL усекается только после того, как SSTable лежит
    // на диске и открывается. Краш на любом шаге до этого оставляет данные в WAL.
    next_seq_ = seq + 1;
    sstables_.push_back({seq, path, std::move(reader)});
    wal_->reset();
    memtable_->clear();

    std::cout << "[Engine] Flush: данные сброшены в " << path << std::endl;
}

void StorageEngine::maybe_start_compaction_locked() {
    if (sstables_.size() <= COMPACTION_TRIGGER) return;
    if (compacting_.exchange(true)) return;

    // compacting_ снимается только после возврата из compact(), поэтому предыдущий
    // поток здесь заведомо вышел из-под всех блокировок и join не может заблокировать.
    if (compaction_thread_.joinable()) compaction_thread_.join();

    compaction_thread_ = std::thread([this] {
        try {
            compact();
        } catch (const std::exception& e) {
            std::cerr << "[Engine] Compaction прерван: " << e.what() << std::endl;
        }
        compacting_.store(false);
    });
}

void StorageEngine::compact() {
    std::lock_guard compaction_guard(compaction_mutex_);

    std::vector<SSTable> snapshot;
    {
        std::shared_lock lock(mutex_);
        if (sstables_.size() < 2) return;
        snapshot = sstables_;
    }

    std::cout << "[Engine] Compaction: слияние " << snapshot.size() << " файлов" << std::endl;

    // Слияние идёт от старых файлов к новым, поэтому более свежая версия ключа
    // затирает предыдущую.
    std::map<std::string, Record> merged;
    for (const auto& table : snapshot) {
        for (auto& [key, record] : table.reader->load_all()) {
            merged[key] = std::move(record);
        }
    }
    // Major compaction сливает все файлы разом, значит более старых версий ключа
    // нигде не осталось и tombstone'ы можно выбросить физически.
    std::erase_if(merged, [](const auto& item) { return item.second.tombstone; });

    // Результат занимает номер самого свежего из слитых файлов: файлы, добавленные
    // флашем во время компакции, получили большие номера и обязаны остаться новее.
    const uint64_t target_seq = snapshot.back().seq;
    const std::string target = sst_path(target_seq);

    SSTableBuilder(target).build(merged);
    auto reader = std::make_shared<SSTableReader>(target);

    std::vector<std::string> obsolete;
    {
        std::unique_lock lock(mutex_);

        std::vector<SSTable> updated;
        updated.push_back({target_seq, target, std::move(reader)});
        for (auto& table : sstables_) {
            if (table.seq > target_seq) {
                updated.push_back(std::move(table)); // добавлен уже после снапшота
            } else if (table.seq != target_seq) {
                obsolete.push_back(table.path);
            }
        }
        sstables_ = std::move(updated);
    }

    // Слитые файлы удаляются уже после подмены списка. Параллельные читатели их
    // не видят, а те, кто ещё держит SSTableReader, работают по своему дескриптору.
    for (const auto& path : obsolete) {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }

    std::cout << "[Engine] Compaction завершён: " << target << " (" << merged.size() << " ключей)"
              << std::endl;
}

size_t StorageEngine::sstable_count() const {
    std::shared_lock lock(mutex_);
    return sstables_.size();
}

size_t StorageEngine::memtable_bytes() const {
    std::shared_lock lock(mutex_);
    return memtable_->approximate_size();
}
