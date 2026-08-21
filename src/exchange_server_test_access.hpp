#pragma once

#include <cstddef>
#include <functional>
#include <vector>

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

struct ExchangeBatchStats {
    std::size_t commands_processed{};
    std::size_t batches_processed{};
    std::vector<std::size_t> batch_size_histogram;
};

class ExchangeServerTestAccess {
public:
    [[nodiscard]] static ExchangeServerRuntimeStats stats(const ExchangeServer& server) noexcept;
    [[nodiscard]] static ExchangeBatchStats batch_stats(const ExchangeServer& server);

    // Called by the sole engine worker immediately before each durable command.
    // Tests must clear this hook before their server is destroyed.
    static void set_before_execute_hook(std::function<void()> hook);
    static void clear_before_execute_hook();

    // Called after the first request has been dequeued and before opportunistic
    // draining begins. This is separate from before_execute to preserve that
    // hook's existing one-call-per-execution semantics.
    static void set_before_batch_drain_hook(std::function<void()> hook);
    static void clear_before_batch_drain_hook();
};

} // namespace testing
} // namespace matching
