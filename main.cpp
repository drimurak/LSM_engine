#include "engine.hpp"

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace {

constexpr uint16_t PORT = 6379;
constexpr size_t MAX_LINE = 1024 * 1024;
constexpr int MAX_CONNECTIONS = 256;

std::atomic<int> active_connections{0};

bool send_all(int socket, const std::string& data) {
    size_t done = 0;
    while (done < data.size()) {
        const ssize_t n = ::send(socket, data.data() + done, data.size() - done, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        done += static_cast<size_t>(n);
    }
    return true;
}

std::string handle_command(const std::string& line, StorageEngine& engine) {
    std::istringstream stream(line);
    std::string command;
    stream >> command;

    try {
        if (command == "SET") {
            std::string key, value;
            stream >> key;
            std::getline(stream >> std::ws, value);
            if (key.empty()) return "ERR SET требует ключ\n";
            engine.put(key, value);
            return "OK\n";
        }
        if (command == "GET") {
            std::string key;
            stream >> key;
            if (key.empty()) return "ERR GET требует ключ\n";
            const auto value = engine.get(key);
            return value.has_value() ? *value + "\n" : "NOT_FOUND\n";
        }
        if (command == "DEL") {
            std::string key;
            stream >> key;
            if (key.empty()) return "ERR DEL требует ключ\n";
            engine.remove(key);
            return "OK\n";
        }
        if (command == "STATS") {
            std::ostringstream out;
            out << "sstables: " << engine.sstable_count() << "\n"
                << "memtable_bytes: " << engine.memtable_bytes() << "\n"
                << "connections: " << active_connections.load() << "\n";
            return out.str();
        }
        if (command == "FLUSH") {
            engine.flush();
            return "OK\n";
        }
        if (command == "COMPACT") {
            engine.compact();
            return "OK\n";
        }
        if (command == "PING") return "PONG\n";
        if (command.empty()) return "ERR пустая команда\n";
    } catch (const std::exception& e) {
        return std::string("ERR ") + e.what() + "\n";
    }

    return "UNKNOWN_COMMAND\n";
}

// Соединение живёт до закрытия клиентом: команда за команду, с корректной сборкой
// строк из потока (один read может принести половину команды или сразу несколько).
void handle_client(int client_socket, StorageEngine& engine) {
    std::string pending;
    char chunk[4096];

    while (true) {
        const ssize_t n = ::recv(client_socket, chunk, sizeof(chunk), 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (n == 0) break;

        pending.append(chunk, static_cast<size_t>(n));
        if (pending.size() > MAX_LINE) {
            send_all(client_socket, "ERR слишком длинная команда\n");
            break;
        }

        size_t newline;
        bool disconnect = false;
        while ((newline = pending.find('\n')) != std::string::npos) {
            std::string line = pending.substr(0, newline);
            pending.erase(0, newline + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();

            if (line == "QUIT" || line == "quit") {
                send_all(client_socket, "BYE\n");
                disconnect = true;
                break;
            }
            if (!send_all(client_socket, handle_command(line, engine))) {
                disconnect = true;
                break;
            }
        }
        if (disconnect) break;
    }

    ::close(client_socket);
    active_connections.fetch_sub(1);
}

} // namespace

int main() {
    // Клиент, отвалившийся посреди ответа, не должен убивать сервер сигналом.
    std::signal(SIGPIPE, SIG_IGN);

    try {
        StorageEngine engine("./data");

        const int server_fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (server_fd < 0) {
            std::cerr << "[Server] Не удалось создать сокет: " << std::strerror(errno) << std::endl;
            return 1;
        }

        int opt = 1;
        ::setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = INADDR_ANY;
        address.sin_port = htons(PORT);

        if (::bind(server_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
            std::cerr << "[Server] bind на порт " << PORT << " не удался: " << std::strerror(errno)
                      << std::endl;
            ::close(server_fd);
            return 1;
        }
        if (::listen(server_fd, 128) < 0) {
            std::cerr << "[Server] listen не удался: " << std::strerror(errno) << std::endl;
            ::close(server_fd);
            return 1;
        }

        std::cout << "[Server] LSM-DB слушает порт " << PORT << std::endl;

        while (true) {
            const int client_socket = ::accept(server_fd, nullptr, nullptr);
            if (client_socket < 0) {
                if (errno == EINTR) continue;
                std::cerr << "[Server] accept: " << std::strerror(errno) << std::endl;
                continue;
            }

            // Поток на соединение без ограничения — это готовый вектор отказа
            // в обслуживании, поэтому число одновременных клиентов ограничено.
            if (active_connections.fetch_add(1) >= MAX_CONNECTIONS) {
                active_connections.fetch_sub(1);
                send_all(client_socket, "ERR слишком много соединений\n");
                ::close(client_socket);
                continue;
            }

            std::thread(handle_client, client_socket, std::ref(engine)).detach();
        }
    } catch (const std::exception& e) {
        std::cerr << "[Server] Критическая ошибка: " << e.what() << std::endl;
        return 1;
    }
}
