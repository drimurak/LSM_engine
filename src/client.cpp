#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

namespace {

const std::string MAGENTA = "\033[35m";
const std::string RESET = "\033[0m";
const std::string RED = "\033[31m";
const std::string GREEN = "\033[32m";
const std::string BLUE = "\033[34m";
const std::string CYAN = "\033[36m";
const std::string BOLD = "\033[1m";

constexpr uint16_t PORT = 6379;

// Одно соединение на всю сессию: переподключение на каждую команду измеряло бы
// скорость установки TCP-сессии, а не скорость хранилища.
class Connection {
public:
    ~Connection() { disconnect(); }

    bool ensure_connected() {
        if (socket_ >= 0) return true;

        socket_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (socket_ < 0) return false;

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(PORT);
        ::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);

        if (::connect(socket_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
            disconnect();
            return false;
        }
        return true;
    }

    void disconnect() {
        if (socket_ >= 0) ::close(socket_);
        socket_ = -1;
        pending_.clear();
    }

    bool send_command(const std::string& command) {
        if (!ensure_connected()) return false;

        const std::string payload = command + "\n";
        size_t done = 0;
        while (done < payload.size()) {
            const ssize_t n = ::send(socket_, payload.data() + done, payload.size() - done, 0);
            if (n < 0) {
                if (errno == EINTR) continue;
                disconnect();
                return false;
            }
            done += static_cast<size_t>(n);
        }
        return true;
    }

    // Ответ всегда завершается переводом строки, поэтому читаем до него, а не
    // фиксированным буфером: один recv может вернуть часть ответа.
    bool read_line(std::string& line) {
        while (true) {
            const size_t newline = pending_.find('\n');
            if (newline != std::string::npos) {
                line = pending_.substr(0, newline);
                pending_.erase(0, newline + 1);
                return true;
            }

            char chunk[4096];
            const ssize_t n = ::recv(socket_, chunk, sizeof(chunk), 0);
            if (n < 0) {
                if (errno == EINTR) continue;
                disconnect();
                return false;
            }
            if (n == 0) {
                disconnect();
                return false;
            }
            pending_.append(chunk, static_cast<size_t>(n));
        }
    }

private:
    int socket_ = -1;
    std::string pending_;
};

void run_command(Connection& connection, const std::string& command) {
    if (!connection.send_command(command)) {
        std::cerr << RED << "Ошибка: сервер не отвечает!" << RESET << std::endl;
        return;
    }

    std::string response;
    if (!connection.read_line(response)) {
        std::cerr << RED << "Ошибка: соединение разорвано." << RESET << std::endl;
        return;
    }

    // STATS отвечает несколькими строками, поэтому дочитываем то, что уже пришло.
    const bool failed = response.rfind("NOT_FOUND", 0) == 0 || response.rfind("ERR", 0) == 0 ||
                        response.rfind("UNKNOWN", 0) == 0;
    std::cout << (failed ? RED : GREEN) << "-> " << response << RESET << std::endl;

    if (command == "STATS") {
        for (int i = 0; i < 2 && connection.read_line(response); ++i) {
            std::cout << GREEN << "   " << response << RESET << std::endl;
        }
    }
}

void run_benchmark(Connection& connection, int count) {
    if (count <= 0) {
        std::cerr << RED << "Некорректное число операций." << RESET << std::endl;
        return;
    }
    std::cout << BLUE << "Запуск бенчмарка: " << count << " записей..." << RESET << std::endl;

    const auto start = std::chrono::steady_clock::now();
    std::string response;
    int failures = 0;

    for (int i = 0; i < count; ++i) {
        const std::string command = "SET key_" + std::to_string(i) + " value_" + std::to_string(i);
        if (!connection.send_command(command) || !connection.read_line(response)) {
            ++failures;
            break;
        }
        if (response != "OK") ++failures;
    }

    const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - start;
    if (failures > 0) {
        std::cout << RED << "Бенчмарк прерван, ошибок: " << failures << RESET << std::endl;
        return;
    }
    std::cout << BOLD << GREEN << "Готово! Время: " << elapsed.count() << " сек. ("
              << static_cast<long long>(count / elapsed.count()) << " оп/сек)" << RESET << std::endl;
}

void print_help() {
    std::cout << CYAN << BOLD << "\nДоступные команды:" << RESET << std::endl;
    std::cout << "  " << BOLD << "SET <key> <value>" << RESET << " - Сохранить значение" << std::endl;
    std::cout << "  " << BOLD << "GET <key>" << RESET << "         - Получить значение" << std::endl;
    std::cout << "  " << BOLD << "DEL <key>" << RESET << "         - Удалить ключ" << std::endl;
    std::cout << "  " << BOLD << "FLUSH" << RESET << "             - Сбросить memtable в SSTable" << std::endl;
    std::cout << "  " << BOLD << "COMPACT" << RESET << "           - Слить SSTable-файлы" << std::endl;
    std::cout << "  " << BOLD << "BENCHMARK <n>" << RESET << "     - Массовая вставка N ключей" << std::endl;
    std::cout << "  " << BOLD << "STATS" << RESET << "             - Показать статистику сервера" << std::endl;
    std::cout << "  " << BOLD << "HELP" << RESET << "              - Показать это сообщение" << std::endl;
    std::cout << "  " << BOLD << "EXIT" << RESET << "              - Выйти\n" << std::endl;
}

} // namespace

int main() {
    std::signal(SIGPIPE, SIG_IGN);

    std::cout << BOLD << MAGENTA << "=== LSM-DB Interactive Client ===" << RESET << std::endl;
    print_help();

    Connection connection;
    std::string line;

    while (true) {
        std::cout << BOLD << "lsm-db> " << RESET << std::flush;
        if (!std::getline(std::cin, line) || line == "EXIT" || line == "exit") break;
        if (line.empty()) continue;

        if (line.rfind("BENCHMARK", 0) == 0) {
            int count = 10000;
            const std::string argument = line.substr(std::string("BENCHMARK").size());
            try {
                if (!argument.empty()) count = std::stoi(argument);
            } catch (const std::exception&) {
                std::cerr << RED << "Не удалось разобрать число операций." << RESET << std::endl;
                continue;
            }
            run_benchmark(connection, count);
        } else if (line == "HELP" || line == "help") {
            print_help();
        } else {
            run_command(connection, line);
        }
    }

    return 0;
}
