#include "kv_store.h"

#include <mutex>
#include <utility>

void KvStore::set(std::string key, std::string value) {
    std::unique_lock lock(mutex_);
    data_[std::move(key)] = std::move(value);
}

std::optional<std::string> KvStore::get(const std::string& key) const {
    std::shared_lock lock(mutex_);
    const auto it = data_.find(key);
    if (it == data_.end()) {
        return std::nullopt;
    }
    return it->second;
}

bool KvStore::erase(const std::string& key) {
    std::unique_lock lock(mutex_);
    return data_.erase(key) > 0;
}

std::size_t KvStore::size() const {
    std::shared_lock lock(mutex_);
    return data_.size();
}
