#include "kv_store.h"
#include "protocol.h"
#include "thread_pool.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

constexpr std::size_t kMaxBufferedBytes = 64 * 1024;
volatile std::sig_atomic_t g_stop_requested = 0;

struct Config {
    std::string host{"127.0.0.1"};
    int port{9090};
    std::size_t threads{4};
    std::size_t queue_capacity{256};
};

struct ServerState {
    KvStore store;
    std::atomic<std::uint64_t> commands{0};
    std::atomic<std::uint64_t> connections{0};
};

void handle_signal(int) {
    g_stop_requested = 1;
}

int parse_int(const char* value, const char* name, int min_value, int max_value) {
    char* end = nullptr;
    errno = 0;
    const long parsed = std::strtol(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || parsed < min_value || parsed > max_value) {
        throw std::invalid_argument(std::string("invalid ") + name);
    }
    return static_cast<int>(parsed);
}

Config parse_args(int argc, char** argv) {
    Config config;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--host" && i + 1 < argc) {
            config.host = argv[++i];
        } else if (arg == "--port" && i + 1 < argc) {
            config.port = parse_int(argv[++i], "port", 1, 65535);
        } else if (arg == "--threads" && i + 1 < argc) {
            config.threads = static_cast<std::size_t>(parse_int(argv[++i], "threads", 1, 1024));
        } else if (arg == "--queue-capacity" && i + 1 < argc) {
            config.queue_capacity = static_cast<std::size_t>(
                parse_int(argv[++i], "queue capacity", 1, 1'000'000));
        } else {
            throw std::invalid_argument("usage: kv_server [--host IP] [--port N] [--threads N] [--queue-capacity N]");
        }
    }
    return config;
}

bool send_all(int fd, const std::string& data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        const ssize_t result = ::send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (result == 0) {
            return false;
        }
        sent += static_cast<std::size_t>(result);
    }
    return true;
}

std::string execute(const Command& command, ServerState& state, bool& should_close) {
    switch (command.type) {
        case CommandType::Ping:
            return "PONG\n";
        case CommandType::Get: {
            const auto value = state.store.get(command.key);
            return value ? "VALUE " + *value + "\n" : "NOT_FOUND\n";
        }
        case CommandType::Set:
            state.store.set(command.key, command.value);
            return "STORED\n";
        case CommandType::Delete:
            return state.store.erase(command.key) ? "DELETED\n" : "NOT_FOUND\n";
        case CommandType::Stats:
            return "STATS keys=" + std::to_string(state.store.size()) +
                   " commands=" + std::to_string(state.commands.load()) +
                   " connections=" + std::to_string(state.connections.load()) + "\n";
        case CommandType::Quit:
            should_close = true;
            return "BYE\n";
        case CommandType::Invalid:
            return "ERROR " + command.error + "\n";
    }
    return "ERROR internal error\n";
}

void handle_client(int client_fd, ServerState& state) {
    state.connections.fetch_add(1, std::memory_order_relaxed);
    std::string buffer;
    char chunk[4096];
    bool should_close = false;

    while (!should_close) {
        const ssize_t received = ::recv(client_fd, chunk, sizeof(chunk), 0);
        if (received == 0) {
            break;
        }
        if (received < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        buffer.append(chunk, static_cast<std::size_t>(received));
        if (buffer.size() > kMaxBufferedBytes) {
            send_all(client_fd, "ERROR request too large\n");
            break;
        }

        std::size_t newline = 0;
        while (!should_close && (newline = buffer.find('\n')) != std::string::npos) {
            std::string line = buffer.substr(0, newline);
            buffer.erase(0, newline + 1);
            state.commands.fetch_add(1, std::memory_order_relaxed);
            if (!send_all(client_fd, execute(parse_command(std::move(line)), state, should_close))) {
                should_close = true;
            }
        }
    }
    ::close(client_fd);
}

int create_listener(const Config& config) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        throw std::runtime_error(std::string("socket: ") + std::strerror(errno));
    }
    const int enabled = 1;
    if (::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled)) < 0) {
        ::close(fd);
        throw std::runtime_error(std::string("setsockopt: ") + std::strerror(errno));
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<std::uint16_t>(config.port));
    if (::inet_pton(AF_INET, config.host.c_str(), &address.sin_addr) != 1) {
        ::close(fd);
        throw std::invalid_argument("host must be an IPv4 address");
    }
    if (::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
        const std::string message = std::string("bind: ") + std::strerror(errno);
        ::close(fd);
        throw std::runtime_error(message);
    }
    if (::listen(fd, 512) < 0) {
        const std::string message = std::string("listen: ") + std::strerror(errno);
        ::close(fd);
        throw std::runtime_error(message);
    }
    return fd;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const Config config = parse_args(argc, argv);
        std::signal(SIGINT, handle_signal);
        std::signal(SIGTERM, handle_signal);
        std::signal(SIGPIPE, SIG_IGN);

        const int listener = create_listener(config);
        ThreadPool pool(config.threads, config.queue_capacity);
        ServerState state;

        std::cout << "listening host=" << config.host << " port=" << config.port
                  << " threads=" << config.threads
                  << " queue_capacity=" << config.queue_capacity << std::endl;

        pollfd descriptor{listener, POLLIN, 0};
        while (!g_stop_requested) {
            const int ready = ::poll(&descriptor, 1, 200);
            if (ready < 0) {
                if (errno == EINTR) {
                    continue;
                }
                throw std::runtime_error(std::string("poll: ") + std::strerror(errno));
            }
            if (ready == 0 || !(descriptor.revents & POLLIN)) {
                continue;
            }

            sockaddr_in peer{};
            socklen_t peer_length = sizeof(peer);
            const int client = ::accept(listener, reinterpret_cast<sockaddr*>(&peer), &peer_length);
            if (client < 0) {
                if (errno == EINTR) {
                    continue;
                }
                throw std::runtime_error(std::string("accept: ") + std::strerror(errno));
            }
            const int enabled = 1;
            ::setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled));
            if (!pool.submit([client, &state] { handle_client(client, state); })) {
                ::close(client);
            }
        }

        ::close(listener);
        pool.stop();
        std::cout << "stopped commands=" << state.commands.load()
                  << " connections=" << state.connections.load() << std::endl;
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
