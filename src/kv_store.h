#pragma once

#include <cstddef>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>

class KvStore {
public:
    void set(std::string key, std::string value);
    [[nodiscard]] std::optional<std::string> get(const std::string& key) const;
    [[nodiscard]] bool erase(const std::string& key);
    [[nodiscard]] std::size_t size() const;

private:
    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, std::string> data_;
};
