#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "kv_store.h"
#include "protocol.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>

namespace {

constexpr std::size_t kMaxBufferedBytes = 64 * 1024;
constexpr std::uint64_t kListenerToken = 0;
volatile std::sig_atomic_t g_stop_requested = 0;

struct Config {
    std::string host{"127.0.0.1"};
    int port{9090};
};

struct Connection {
    int fd;
    std::string input;
    std::string output;
    std::size_t output_offset{0};
    bool close_after_write{false};
};

void handle_signal(int) { g_stop_requested = 1; }

int parse_int(const char* text) {
    char* end = nullptr;
    errno = 0;
    const long value = std::strtol(text, &end, 10);
    if (errno || end == text || *end != '\0' || value < 1 || value > 65535) {
        throw std::invalid_argument("invalid port");
    }
    return static_cast<int>(value);
}

Config parse_args(int argc, char** argv) {
    Config config;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--host" && i + 1 < argc) config.host = argv[++i];
        else if (arg == "--port" && i + 1 < argc) config.port = parse_int(argv[++i]);
        else throw std::invalid_argument("usage: kv_server_epoll [--host IP] [--port N]");
    }
    return config;
}

int create_listener(const Config& config) {
    // Non-blocking listen is essential: after readiness, accept until EAGAIN.
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) throw std::runtime_error(std::string("socket: ") + std::strerror(errno));
    const int enabled = 1;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<std::uint16_t>(config.port));
    if (::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled)) < 0 ||
        ::inet_pton(AF_INET, config.host.c_str(), &address.sin_addr) != 1 ||
        ::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0 ||
        ::listen(fd, 512) < 0) {
        const std::string error = std::strerror(errno);
        ::close(fd);
        throw std::runtime_error("listener setup: " + error);
    }
    return fd;
}

class Server {
public:
    explicit Server(const Config& config) : listener_(create_listener(config)) {
        epoll_fd_ = ::epoll_create1(EPOLL_CLOEXEC);
        if (epoll_fd_ < 0) {
            const std::string error = std::strerror(errno);
            ::close(listener_);
            throw std::runtime_error("epoll_create1: " + error);
        }
        epoll_event event{};
        event.events = EPOLLIN;
        event.data.u64 = kListenerToken;
        if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, listener_, &event) < 0) {
            const std::string error = std::strerror(errno);
            ::close(epoll_fd_);
            ::close(listener_);
            throw std::runtime_error("epoll_ctl listener: " + error);
        }
    }

    ~Server() {
        for (const auto& [token, connection] : connections_) {
            (void)token;
            ::close(connection.fd);
        }
        if (epoll_fd_ >= 0) ::close(epoll_fd_);
        ::close(listener_);
    }

    void run(const Config& config) {
        std::cout << "listening host=" << config.host << " port=" << config.port << " mode=epoll" << std::endl;
        std::array<epoll_event, 256> events{};
        while (!g_stop_requested) {
            // Level-triggered readiness remains reported until the socket is drained.
            const int count = ::epoll_wait(epoll_fd_, events.data(), static_cast<int>(events.size()), 200);
            if (count < 0) {
                if (errno == EINTR) continue;
                throw std::runtime_error(std::string("epoll_wait: ") + std::strerror(errno));
            }
            for (int i = 0; i < count && !g_stop_requested; ++i) {
                const auto token = events[static_cast<std::size_t>(i)].data.u64;
                if (token == kListenerToken) {
                    accept_ready();
                    continue;
                }
                auto it = connections_.find(token);
                if (it == connections_.end()) continue; // Stale event, possibly after fd reuse.
                Connection& connection = it->second;
                const auto flags = events[static_cast<std::size_t>(i)].events;
                bool ok = !(flags & EPOLLERR);
                if (ok && (flags & (EPOLLIN | EPOLLRDHUP | EPOLLHUP))) ok = read_ready(connection);
                if (ok && (flags & EPOLLOUT || !connection.output.empty())) ok = write_ready(connection);
                if (!ok || (connection.close_after_write && connection.output.empty())) {
                    close_connection(token);
                } else {
                    update_interest(connection, token);
                }
            }
        }
        std::cout << "stopped commands=" << commands_ << " connections=" << accepted_connections_ << std::endl;
    }

private:
    void accept_ready() {
        while (true) {
            // accept4 atomically applies non-blocking and close-on-exec to each client.
            const int fd = ::accept4(listener_, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
            if (fd < 0) {
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) return;
                throw std::runtime_error(std::string("accept4: ") + std::strerror(errno));
            }
            const int enabled = 1;
            ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled));
            const std::uint64_t token = next_token_++;
            epoll_event event{};
            event.events = EPOLLIN | EPOLLRDHUP;
            event.data.u64 = token;
            if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &event) < 0) {
                ::close(fd);
                throw std::runtime_error(std::string("epoll_ctl client: ") + std::strerror(errno));
            }
            connections_.emplace(token, Connection{fd, {}, {}, 0, false});
            ++accepted_connections_;
        }
    }

    std::string execute(const Command& command, bool& close_after_write) {
        switch (command.type) {
            case CommandType::Ping: return "PONG\n";
            case CommandType::Get: {
                const auto value = store_.get(command.key);
                return value ? "VALUE " + *value + "\n" : "NOT_FOUND\n";
            }
            case CommandType::Set:
                store_.set(command.key, command.value);
                return "STORED\n";
            case CommandType::Delete:
                return store_.erase(command.key) ? "DELETED\n" : "NOT_FOUND\n";
            case CommandType::Stats:
                return "STATS keys=" + std::to_string(store_.size()) +
                       " commands=" + std::to_string(commands_) +
                       " connections=" + std::to_string(accepted_connections_) + "\n";
            case CommandType::Quit:
                close_after_write = true;
                return "BYE\n";
            case CommandType::Invalid:
                return "ERROR " + command.error + "\n";
        }
        return "ERROR internal error\n";
    }

    bool read_ready(Connection& connection) {
        char chunk[4096];
        while (!connection.close_after_write) {
            const ssize_t count = ::recv(connection.fd, chunk, sizeof(chunk), 0);
            if (count > 0) {
                connection.input.append(chunk, static_cast<std::size_t>(count));
                if (connection.input.size() > kMaxBufferedBytes) {
                    connection.output += "ERROR request too large\n";
                    connection.close_after_write = true;
                    break;
                }
                std::size_t newline;
                while (!connection.close_after_write &&
                       (newline = connection.input.find('\n')) != std::string::npos) {
                    std::string line = connection.input.substr(0, newline);
                    connection.input.erase(0, newline + 1);
                    ++commands_;
                    connection.output += execute(parse_command(std::move(line)), connection.close_after_write);
                }
                continue;
            }
            if (count == 0) {
                connection.close_after_write = true;
                break;
            }
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            return false;
        }
        return true;
    }

    bool write_ready(Connection& connection) {
        while (connection.output_offset < connection.output.size()) {
            // send may write only a prefix; retain the remainder for EPOLLOUT.
            const ssize_t count = ::send(connection.fd,
                                         connection.output.data() + connection.output_offset,
                                         connection.output.size() - connection.output_offset,
                                         MSG_NOSIGNAL);
            if (count > 0) {
                connection.output_offset += static_cast<std::size_t>(count);
                continue;
            }
            if (count == 0) return false;
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return true;
            return false;
        }
        connection.output.clear();
        connection.output_offset = 0;
        return true;
    }

    void update_interest(const Connection& connection, std::uint64_t token) {
        epoll_event event{};
        // Once no more input is wanted, omit persistent RDHUP readiness while
        // waiting for a slow peer to accept the final queued response.
        event.events = connection.close_after_write ? 0U : (EPOLLIN | EPOLLRDHUP);
        if (!connection.output.empty()) event.events |= EPOLLOUT;
        event.data.u64 = token;
        if (::epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, connection.fd, &event) < 0) {
            throw std::runtime_error(std::string("epoll_ctl modify: ") + std::strerror(errno));
        }
    }

    void close_connection(std::uint64_t token) {
        const int fd = connections_.at(token).fd;
        ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
        ::close(fd);
        connections_.erase(token);
    }

    int listener_;
    int epoll_fd_{-1};
    std::uint64_t next_token_{1};
    std::uint64_t commands_{0};
    std::uint64_t accepted_connections_{0};
    KvStore store_;
    std::unordered_map<std::uint64_t, Connection> connections_;
};

} // namespace

int main(int argc, char** argv) {
    try {
        const Config config = parse_args(argc, argv);
        std::signal(SIGINT, handle_signal);
        std::signal(SIGTERM, handle_signal);
        std::signal(SIGPIPE, SIG_IGN);
        Server server(config);
        server.run(config);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
