#include "kv_store.h"
#include "protocol.h"
#include "thread_pool.h"

#include <atomic>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

void expect(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void test_protocol_valid_commands() {
    expect(parse_command("PING").type == CommandType::Ping, "PING parsing");
    expect(parse_command("GET alpha").type == CommandType::Get, "GET parsing");
    const Command set = parse_command("SET alpha value with spaces");
    expect(set.type == CommandType::Set && set.key == "alpha" && set.value == "value with spaces", "SET parsing");
    expect(parse_command("DEL alpha").type == CommandType::Delete, "DEL parsing");
    expect(parse_command("STATS\r").type == CommandType::Stats, "CRLF parsing");
    expect(parse_command("QUIT").type == CommandType::Quit, "QUIT parsing");
}

void test_protocol_invalid_commands() {
    expect(parse_command("").type == CommandType::Invalid, "empty input");
    expect(parse_command("GET").type == CommandType::Invalid, "missing GET key");
    expect(parse_command("SET key").type == CommandType::Invalid, "missing SET value");
    expect(parse_command("PING extra").type == CommandType::Invalid, "extra PING argument");
    expect(parse_command("UNKNOWN").type == CommandType::Invalid, "unknown command");
}

void test_store_crud() {
    KvStore store;
    expect(store.size() == 0, "empty store");
    store.set("key", "value");
    expect(store.get("key") == "value", "stored value");
    store.set("key", "updated");
    expect(store.get("key") == "updated", "updated value");
    expect(store.erase("key"), "delete existing key");
    expect(!store.get("key").has_value(), "deleted key absent");
    expect(!store.erase("key"), "delete missing key");
}

void test_store_concurrent_access() {
    KvStore store;
    constexpr int kThreads = 8;
    constexpr int kKeysPerThread = 1000;
    std::vector<std::thread> workers;
    for (int thread_id = 0; thread_id < kThreads; ++thread_id) {
        workers.emplace_back([&, thread_id] {
            for (int i = 0; i < kKeysPerThread; ++i) {
                const std::string key = std::to_string(thread_id) + ":" + std::to_string(i);
                store.set(key, "value");
                expect(store.get(key) == "value", "concurrent read after write");
            }
        });
    }
    for (auto& worker : workers) worker.join();
    expect(store.size() == static_cast<std::size_t>(kThreads * kKeysPerThread), "concurrent key count");
}

void test_thread_pool_executes_tasks() {
    constexpr int kTaskCount = 5000;
    std::atomic<int> completed{0};
    ThreadPool pool(4, 32);
    for (int i = 0; i < kTaskCount; ++i) {
        expect(pool.submit([&completed] { completed.fetch_add(1, std::memory_order_relaxed); }), "task submission");
    }
    pool.stop();
    expect(completed.load() == kTaskCount, "all queued tasks completed");
}

}  // namespace

int main() {
    const std::vector<std::pair<std::string, void (*)()>> tests = {
        {"protocol_valid_commands", test_protocol_valid_commands},
        {"protocol_invalid_commands", test_protocol_invalid_commands},
        {"store_crud", test_store_crud},
        {"store_concurrent_access", test_store_concurrent_access},
        {"thread_pool_executes_tasks", test_thread_pool_executes_tasks},
    };

    int passed = 0;
    for (const auto& [name, test] : tests) {
        try {
            test();
            ++passed;
            std::cout << "PASS " << name << '\n';
        } catch (const std::exception& error) {
            std::cerr << "FAIL " << name << ": " << error.what() << '\n';
            return 1;
        }
    }
    std::cout << "unit_tests_passed=" << passed << '\n';
    return 0;
}
