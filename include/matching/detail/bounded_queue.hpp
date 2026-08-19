#pragma once

#include <condition_variable>
#include <concepts>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <utility>

namespace matching::gateway_detail {

template <typename T>
class BoundedQueue {
public:
    explicit BoundedQueue(std::size_t capacity) : capacity_(capacity) {}

    BoundedQueue(const BoundedQueue&) = delete;
    BoundedQueue& operator=(const BoundedQueue&) = delete;

    template <typename U>
        requires std::constructible_from<T, U&&>
    [[nodiscard]] bool try_push(U&& value) {
        std::unique_lock lock(mutex_);
        if (closed_ || items_.size() == capacity_) {
            return false;
        }
        items_.emplace_back(std::forward<U>(value));
        lock.unlock();
        not_empty_.notify_one();
        return true;
    }

    [[nodiscard]] std::optional<T> try_pop() {
        std::lock_guard lock(mutex_);
        if (items_.empty()) {
            return std::nullopt;
        }
        T value = std::move(items_.front());
        items_.pop_front();
        return value;
    }

    [[nodiscard]] std::optional<T> wait_pop() {
        std::unique_lock lock(mutex_);
        not_empty_.wait(lock, [this] { return closed_ || !items_.empty(); });
        if (items_.empty()) {
            return std::nullopt;
        }
        T value = std::move(items_.front());
        items_.pop_front();
        return value;
    }

    void close() noexcept {
        {
            std::lock_guard lock(mutex_);
            if (closed_) {
                return;
            }
            closed_ = true;
        }
        not_empty_.notify_all();
    }

    [[nodiscard]] bool closed() const noexcept {
        std::lock_guard lock(mutex_);
        return closed_;
    }

    [[nodiscard]] std::size_t size() const noexcept {
        std::lock_guard lock(mutex_);
        return items_.size();
    }

    [[nodiscard]] std::size_t capacity() const noexcept {
        return capacity_;
    }

private:
    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::condition_variable not_empty_;
    std::deque<T> items_;
    bool closed_{};
};

} // namespace matching::gateway_detail
