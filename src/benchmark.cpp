#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

struct Config {
    std::string host{"127.0.0.1"};
    int port{9090};
    int clients{8};
    int requests_per_client{5000};
    int read_ratio{80};
    int keyspace{1000};
    int value_size{32};
    int warmup{100};
};

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
        if (i + 1 >= argc) {
            throw std::invalid_argument("every option requires a value");
        }
        const char* value = argv[++i];
        if (arg == "--host") config.host = value;
        else if (arg == "--port") config.port = parse_int(value, "port", 1, 65535);
        else if (arg == "--clients") config.clients = parse_int(value, "clients", 1, 2048);
        else if (arg == "--requests-per-client") config.requests_per_client = parse_int(value, "requests", 1, 10'000'000);
        else if (arg == "--read-ratio") config.read_ratio = parse_int(value, "read ratio", 0, 100);
        else if (arg == "--keyspace") config.keyspace = parse_int(value, "keyspace", 1, 10'000'000);
        else if (arg == "--value-size") config.value_size = parse_int(value, "value size", 1, 4096);
        else if (arg == "--warmup") config.warmup = parse_int(value, "warmup", 0, 1'000'000);
        else throw std::invalid_argument("unknown option: " + arg);
    }
    return config;
}

class Connection {
public:
    Connection(const std::string& host, int port) {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0) throw std::runtime_error("socket failed");
        const int enabled = 1;
        ::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled));

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(static_cast<std::uint16_t>(port));
        if (::inet_pton(AF_INET, host.c_str(), &address.sin_addr) != 1 ||
            ::connect(fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
            const std::string message = std::string("connect failed: ") + std::strerror(errno);
            ::close(fd_);
            fd_ = -1;
            throw std::runtime_error(message);
        }
    }

    ~Connection() {
        if (fd_ >= 0) ::close(fd_);
    }

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    std::string request(const std::string& command) {
        std::size_t sent = 0;
        while (sent < command.size()) {
            const ssize_t result = ::send(fd_, command.data() + sent, command.size() - sent, MSG_NOSIGNAL);
            if (result <= 0) throw std::runtime_error("send failed");
            sent += static_cast<std::size_t>(result);
        }

        while (true) {
            const auto newline = receive_buffer_.find('\n');
            if (newline != std::string::npos) {
                std::string response = receive_buffer_.substr(0, newline);
                receive_buffer_.erase(0, newline + 1);
                return response;
            }
            char chunk[1024];
            const ssize_t received = ::recv(fd_, chunk, sizeof(chunk), 0);
            if (received <= 0) throw std::runtime_error("receive failed");
            receive_buffer_.append(chunk, static_cast<std::size_t>(received));
        }
    }

private:
    int fd_{-1};
    std::string receive_buffer_;
};

std::uint64_t next_random(std::uint64_t& state) {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
}

double percentile(const std::vector<double>& sorted, double fraction) {
    if (sorted.empty()) return 0.0;
    const double position = fraction * static_cast<double>(sorted.size() - 1);
    const auto lower = static_cast<std::size_t>(std::floor(position));
    const auto upper = static_cast<std::size_t>(std::ceil(position));
    if (lower == upper) return sorted[lower];
    const double weight = position - static_cast<double>(lower);
    return sorted[lower] * (1.0 - weight) + sorted[upper] * weight;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        ::signal(SIGPIPE, SIG_IGN);
        const Config config = parse_args(argc, argv);
        std::vector<std::vector<double>> per_client(static_cast<std::size_t>(config.clients));
        std::atomic<int> errors{0};
        const std::string value(static_cast<std::size_t>(config.value_size), 'x');

        const auto started = std::chrono::steady_clock::now();
        std::vector<std::thread> threads;
        threads.reserve(static_cast<std::size_t>(config.clients));
        for (int client_id = 0; client_id < config.clients; ++client_id) {
            threads.emplace_back([&, client_id] {
                try {
                    Connection connection(config.host, config.port);
                    for (int i = 0; i < config.warmup; ++i) {
                        if (connection.request("PING\n") != "PONG") ++errors;
                    }

                    auto& latencies = per_client[static_cast<std::size_t>(client_id)];
                    latencies.reserve(static_cast<std::size_t>(config.requests_per_client));
                    std::uint64_t random_state = 0x9e3779b97f4a7c15ULL ^
                                                 static_cast<std::uint64_t>(client_id + 1);
                    for (int i = 0; i < config.requests_per_client; ++i) {
                        const auto random = next_random(random_state);
                        const int key_id = static_cast<int>(random % static_cast<std::uint64_t>(config.keyspace));
                        const bool is_read = static_cast<int>((random >> 32) % 100) < config.read_ratio;
                        const std::string command = is_read
                            ? "GET key" + std::to_string(key_id) + "\n"
                            : "SET key" + std::to_string(key_id) + " " + value + "\n";

                        const auto before = std::chrono::steady_clock::now();
                        const std::string response = connection.request(command);
                        const auto after = std::chrono::steady_clock::now();
                        const bool valid = is_read
                            ? (response == "NOT_FOUND" || response.rfind("VALUE ", 0) == 0)
                            : response == "STORED";
                        if (!valid) ++errors;
                        latencies.push_back(
                            std::chrono::duration<double, std::micro>(after - before).count());
                    }
                } catch (const std::exception&) {
                    ++errors;
                }
            });
        }
        for (auto& thread : threads) thread.join();
        const auto finished = std::chrono::steady_clock::now();

        std::vector<double> latencies;
        for (auto& client_latencies : per_client) {
            latencies.insert(latencies.end(), client_latencies.begin(), client_latencies.end());
        }
        std::sort(latencies.begin(), latencies.end());
        const double seconds = std::chrono::duration<double>(finished - started).count();
        const double throughput = seconds > 0.0 ? static_cast<double>(latencies.size()) / seconds : 0.0;

        std::cout << std::fixed << std::setprecision(3)
                  << "{\"clients\":" << config.clients
                  << ",\"requests_per_client\":" << config.requests_per_client
                  << ",\"total_requests\":" << latencies.size()
                  << ",\"read_ratio\":" << config.read_ratio
                  << ",\"elapsed_seconds\":" << seconds
                  << ",\"throughput_ops_per_sec\":" << throughput
                  << ",\"p50_latency_us\":" << percentile(latencies, 0.50)
                  << ",\"p95_latency_us\":" << percentile(latencies, 0.95)
                  << ",\"p99_latency_us\":" << percentile(latencies, 0.99)
                  << ",\"errors\":" << errors.load() << "}\n";
        return errors.load() == 0 ? 0 : 2;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
