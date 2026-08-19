#pragma once

#include <cstddef>
#include <functional>

namespace matching {
class ExchangeServer;

namespace testing {

// Source-tree-only controls and observations for deterministic concurrency
// tests. They are intentionally not installed under include/matching.
struct ExchangeServerRuntimeStats {
    std::size_t in_flight{};
    std::size_t maximum_observed_in_flight{};
    std::size_t request_queue_size{};
    std::size_t request_queue_capacity{};
    std::size_t response_queue_size{};
    std::size_t response_queue_capacity{};
};

class ExchangeServerTestAccess {
public:
    [[nodiscard]] static ExchangeServerRuntimeStats stats(const ExchangeServer& server) noexcept;

    // Called by the sole engine worker immediately before each durable command.
    // Tests must clear this hook before their server is destroyed.
    static void set_before_execute_hook(std::function<void()> hook);
    static void clear_before_execute_hook();
};

} // namespace testing
} // namespace matching
