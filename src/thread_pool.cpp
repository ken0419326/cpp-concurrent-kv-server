#include "thread_pool.h"

#include <stdexcept>
#include <utility>

ThreadPool::ThreadPool(std::size_t worker_count, std::size_t queue_capacity)
    : queue_capacity_(queue_capacity) {
    if (worker_count == 0 || queue_capacity == 0) {
        throw std::invalid_argument("worker count and queue capacity must be positive");
    }
    workers_.reserve(worker_count);
    for (std::size_t i = 0; i < worker_count; ++i) {
        workers_.emplace_back(&ThreadPool::worker_loop, this);
    }
}

ThreadPool::~ThreadPool() {
    stop();
}

bool ThreadPool::submit(std::function<void()> task) {
    std::unique_lock lock(mutex_);
    not_full_.wait(lock, [this] {
        return stopping_ || tasks_.size() < queue_capacity_;
    });
    if (stopping_) {
        return false;
    }
    tasks_.push(std::move(task));
    not_empty_.notify_one();
    return true;
}

void ThreadPool::stop() {
    {
        std::lock_guard lock(mutex_);
        if (stopping_) {
            return;
        }
        stopping_ = true;
    }
    not_empty_.notify_all();
    not_full_.notify_all();
    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

void ThreadPool::worker_loop() {
    while (true) {
        std::function<void()> task;
        {
            std::unique_lock lock(mutex_);
            not_empty_.wait(lock, [this] {
                return stopping_ || !tasks_.empty();
            });
            if (stopping_ && tasks_.empty()) {
                return;
            }
            task = std::move(tasks_.front());
            tasks_.pop();
            not_full_.notify_one();
        }
        task();
    }
}
