#include "engine.hpp"

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const std::string& what) {
    if (condition) return;
    std::cerr << "  ПРОВАЛ: " << what << std::endl;
    ++failures;
}

void check_value(const std::optional<std::string>& actual, const std::string& expected,
                 const std::string& what) {
    if (actual.has_value() && *actual == expected) return;
    std::cerr << "  ПРОВАЛ: " << what << " (получено: "
              << (actual.has_value() ? *actual : std::string("<нет значения>")) << ", ожидалось: "
              << expected << ")" << std::endl;
    ++failures;
}

class TempDir {
public:
    explicit TempDir(const std::string& name)
        : path_((std::filesystem::temp_directory_path() / ("lsm_test_" + name)).string()) {
        std::filesystem::remove_all(path_);
    }
    ~TempDir() { std::filesystem::remove_all(path_); }

    const std::string& path() const { return path_; }

private:
    std::string path_;
};

void test_roundtrip_through_flush() {
    std::cout << "roundtrip через flush" << std::endl;
    TempDir dir("roundtrip");

    StorageEngine engine(dir.path());
    for (int i = 0; i < 500; ++i) {
        engine.put("key_" + std::to_string(i), "value_" + std::to_string(i));
    }
    engine.flush();
    check(engine.sstable_count() == 1, "после flush должен появиться ровно один SSTable");

    for (int i = 0; i < 500; ++i) {
        check_value(engine.get("key_" + std::to_string(i)), "value_" + std::to_string(i),
                    "ключ key_" + std::to_string(i) + " читается с диска");
    }
    check(!engine.get("отсутствующий").has_value(), "несуществующий ключ не находится");
}

void test_delete_and_tombstones() {
    std::cout << "удаление и tombstone'ы" << std::endl;
    TempDir dir("delete");

    StorageEngine engine(dir.path());
    engine.put("alpha", "1");
    engine.put("beta", "2");
    engine.flush();

    engine.remove("alpha");
    check(!engine.get("alpha").has_value(), "удалённый ключ не виден до flush");

    engine.flush();
    check(!engine.get("alpha").has_value(), "tombstone перекрывает старое значение в SSTable");
    check_value(engine.get("beta"), "2", "соседний ключ не пострадал");

    engine.put("alpha", "3");
    check_value(engine.get("alpha"), "3", "ключ можно записать заново после удаления");
}

void test_value_colliding_with_old_tombstone_marker() {
    std::cout << "значение, совпадающее со старым маркером удаления" << std::endl;
    TempDir dir("marker");

    StorageEngine engine(dir.path());
    engine.put("key", "__DELETED__");
    engine.flush();
    check_value(engine.get("key"), "__DELETED__", "строка __DELETED__ остаётся обычным значением");
}

void test_recovery_after_crash() {
    std::cout << "восстановление после аварийного завершения" << std::endl;
    TempDir dir("recovery");

    {
        StorageEngine engine(dir.path());
        engine.put("k1", "v1");
        engine.put("k2", "v2");
        engine.remove("k1");
        // Деструктор не вызывает flush — данные обязаны восстановиться из WAL.
    }
    {
        StorageEngine engine(dir.path());
        check(!engine.get("k1").has_value(), "удаление пережило перезапуск");
        check_value(engine.get("k2"), "v2", "запись пережила перезапуск");
    }
}

void test_recovery_of_flushed_data() {
    std::cout << "перезапуск поверх существующих SSTable" << std::endl;
    TempDir dir("restart");

    {
        StorageEngine engine(dir.path());
        for (int i = 0; i < 200; ++i) engine.put("k" + std::to_string(i), "v" + std::to_string(i));
        engine.flush();
        engine.put("после_flush", "ok");
    }
    {
        StorageEngine engine(dir.path());
        check(engine.sstable_count() == 1, "существующий SSTable найден при старте");
        for (int i = 0; i < 200; ++i) {
            check_value(engine.get("k" + std::to_string(i)), "v" + std::to_string(i),
                        "ключ k" + std::to_string(i) + " читается после перезапуска");
        }
        check_value(engine.get("после_flush"), "ok", "запись после flush восстановлена из WAL");
    }
}

void test_truncated_wal_tail() {
    std::cout << "оборванный хвост WAL" << std::endl;
    TempDir dir("torn");

    {
        StorageEngine engine(dir.path());
        engine.put("целая", "запись");
    }
    {
        // Имитация краха посреди записи: к журналу дописан неполный мусорный хвост.
        const std::string wal = dir.path() + "/wal.log";
        std::FILE* file = std::fopen(wal.c_str(), "ab");
        check(file != nullptr, "WAL открывается для дозаписи");
        const char garbage[] = {0x11, 0x22, 0x33, 0x44, 0x05, 0x00};
        std::fwrite(garbage, 1, sizeof(garbage), file);
        std::fclose(file);
    }
    {
        StorageEngine engine(dir.path());
        check_value(engine.get("целая"), "запись", "запись до обрыва восстановлена");
    }
}

void test_compaction() {
    std::cout << "компакция" << std::endl;
    TempDir dir("compaction");

    StorageEngine engine(dir.path());
    for (int generation = 0; generation < 4; ++generation) {
        for (int i = 0; i < 100; ++i) {
            engine.put("k" + std::to_string(i), "gen" + std::to_string(generation));
        }
        engine.flush();
    }
    engine.remove("k7");
    engine.flush();
    check(engine.sstable_count() == 5, "создано пять SSTable-файлов");

    engine.compact();
    check(engine.sstable_count() == 1, "после компакции остаётся один файл");

    for (int i = 0; i < 100; ++i) {
        if (i == 7) continue;
        check_value(engine.get("k" + std::to_string(i)), "gen3",
                    "актуальной остаётся последняя версия ключа k" + std::to_string(i));
    }
    check(!engine.get("k7").has_value(), "удалённый ключ не воскресает после компакции");

    int files = 0;
    for (const auto& entry : std::filesystem::directory_iterator(dir.path())) {
        if (entry.path().extension() == ".sst") ++files;
    }
    check(files == 1, "лишние файлы удалены с диска");
}

void test_compaction_survives_restart() {
    std::cout << "перезапуск после компакции" << std::endl;
    TempDir dir("compaction_restart");

    {
        StorageEngine engine(dir.path());
        for (int generation = 0; generation < 3; ++generation) {
            for (int i = 0; i < 50; ++i) {
                engine.put("k" + std::to_string(i), "gen" + std::to_string(generation));
            }
            engine.flush();
        }
        engine.compact();
    }
    {
        StorageEngine engine(dir.path());
        for (int i = 0; i < 50; ++i) {
            check_value(engine.get("k" + std::to_string(i)), "gen2",
                        "сжатые данные читаются после перезапуска");
        }
    }
}

void test_large_records() {
    std::cout << "крупные ключи и значения" << std::endl;
    TempDir dir("large");

    StorageEngine engine(dir.path());
    const std::string long_key(4096, 'k');
    const std::string long_value(512 * 1024, 'v');

    engine.put(long_key, long_value);
    engine.put("пустое", "");
    engine.flush();

    check_value(engine.get(long_key), long_value, "значение на 512 КиБ читается целиком");
    check_value(engine.get("пустое"), "", "пустое значение отличается от отсутствующего");

    bool rejected = false;
    try {
        engine.put("", "значение");
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    check(rejected, "пустой ключ отвергается");
}

void test_concurrent_access() {
    std::cout << "конкурентный доступ" << std::endl;
    TempDir dir("concurrent");

    StorageEngine engine(dir.path());
    constexpr int WRITERS = 4;
    constexpr int PER_WRITER = 400;

    std::atomic<bool> stop{false};
    std::atomic<int> read_errors{0};

    std::vector<std::thread> threads;
    for (int w = 0; w < WRITERS; ++w) {
        threads.emplace_back([&engine, w] {
            for (int i = 0; i < PER_WRITER; ++i) {
                engine.put("w" + std::to_string(w) + "_k" + std::to_string(i), std::string(256, 'x'));
            }
        });
    }
    for (int r = 0; r < 2; ++r) {
        threads.emplace_back([&engine, &stop, &read_errors] {
            while (!stop.load()) {
                try {
                    (void)engine.get("w0_k0");
                } catch (const std::exception&) {
                    read_errors.fetch_add(1);
                }
            }
        });
    }

    for (int i = 0; i < WRITERS; ++i) threads[i].join();
    stop.store(true);
    for (size_t i = WRITERS; i < threads.size(); ++i) threads[i].join();

    check(read_errors.load() == 0, "параллельные чтения не приводят к ошибкам");
    for (int w = 0; w < WRITERS; ++w) {
        for (int i = 0; i < PER_WRITER; i += 97) {
            check_value(engine.get("w" + std::to_string(w) + "_k" + std::to_string(i)),
                        std::string(256, 'x'), "запись из параллельного потока не потеряна");
        }
    }
}

// Компакция работает со снапшотом списка файлов, поэтому SSTable, созданные флашем
// уже во время слияния, не участвуют в нём и не должны быть удалены вместе со слитыми.
void test_flush_during_compaction() {
    std::cout << "flush параллельно с компакцией" << std::endl;
    TempDir dir("flush_during_compaction");

    StorageEngine engine(dir.path());
    for (int generation = 0; generation < 6; ++generation) {
        for (int i = 0; i < 200; ++i) {
            engine.put("old" + std::to_string(i), "gen" + std::to_string(generation));
        }
        engine.flush();
    }

    std::thread compaction([&engine] { engine.compact(); });
    for (int i = 0; i < 200; ++i) {
        engine.put("new" + std::to_string(i), "свежее");
        if (i % 50 == 0) engine.flush();
    }
    engine.flush();
    compaction.join();

    for (int i = 0; i < 200; ++i) {
        check_value(engine.get("new" + std::to_string(i)), "свежее",
                    "запись во время компакции не потеряна");
        check_value(engine.get("old" + std::to_string(i)), "gen5",
                    "слитые данные остались актуальными");
    }
}

void test_rejects_foreign_file() {
    std::cout << "отказ читать файл чужого формата" << std::endl;
    TempDir dir("foreign");
    std::filesystem::create_directories(dir.path());

    const std::string path = dir.path() + "/sst_00000000000000000001.sst";
    std::FILE* file = std::fopen(path.c_str(), "wb");
    check(file != nullptr, "тестовый файл создаётся");
    const char junk[64] = {};
    std::fwrite(junk, 1, sizeof(junk), file);
    std::fclose(file);

    bool rejected = false;
    try {
        StorageEngine engine(dir.path());
    } catch (const std::exception&) {
        rejected = true;
    }
    check(rejected, "файл с чужой сигнатурой отвергается, а не читается как мусор");
}

} // namespace

int main() {
    test_roundtrip_through_flush();
    test_delete_and_tombstones();
    test_value_colliding_with_old_tombstone_marker();
    test_recovery_after_crash();
    test_recovery_of_flushed_data();
    test_truncated_wal_tail();
    test_compaction();
    test_compaction_survives_restart();
    test_large_records();
    test_concurrent_access();
    test_flush_during_compaction();
    test_rejects_foreign_file();

    if (failures > 0) {
        std::cerr << "\nПровалено проверок: " << failures << std::endl;
        return 1;
    }
    std::cout << "\nВсе проверки пройдены." << std::endl;
    return 0;
}
