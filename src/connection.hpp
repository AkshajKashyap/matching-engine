#pragma once

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace matching::gateway_detail {

enum class OutputFlushResult : std::uint8_t {
    Drained,
    WouldBlock,
    Fatal,
};

class ConnectionOutputBuffer {
public:
    [[nodiscard]] bool append(std::span<const std::byte> bytes, std::size_t maximum_pending_bytes) {
        const std::size_t pending = pending_size();
        if (pending > maximum_pending_bytes || bytes.size() > maximum_pending_bytes - pending) {
            return false;
        }
        compact();
        bytes_.insert(bytes_.end(), bytes.begin(), bytes.end());
        return true;
    }

    [[nodiscard]] bool empty() const noexcept {
        return pending_size() == 0;
    }

    [[nodiscard]] std::size_t pending_size() const noexcept {
        return bytes_.size() - offset_;
    }

    template <typename SendFunction>
    [[nodiscard]] OutputFlushResult flush(SendFunction&& send_function) {
        while (!empty()) {
            const auto result = send_function(bytes_.data() + offset_, pending_size());
            if (result > 0) {
                const std::size_t sent = static_cast<std::size_t>(result);
                if (sent > pending_size()) {
                    return OutputFlushResult::Fatal;
                }
                offset_ += sent;
                if (empty()) {
                    bytes_.clear();
                    offset_ = 0;
                    return OutputFlushResult::Drained;
                }
                continue;
            }
            if (result == 0) {
                return OutputFlushResult::Fatal;
            }
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return OutputFlushResult::WouldBlock;
            }
            return OutputFlushResult::Fatal;
        }
        return OutputFlushResult::Drained;
    }

private:
    void compact() {
        if (offset_ == 0) {
            return;
        }
        if (offset_ == bytes_.size()) {
            bytes_.clear();
            offset_ = 0;
            return;
        }
        bytes_.erase(bytes_.begin(), bytes_.begin() + static_cast<std::ptrdiff_t>(offset_));
        offset_ = 0;
    }

    std::vector<std::byte> bytes_;
    std::size_t offset_{};
};

} // namespace matching::gateway_detail
