#include "bounded_queue.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <future>
#include <latch>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

namespace matching::gateway_detail {
namespace {

using namespace std::chrono_literals;

TEST(BoundedQueueTest, PreservesFifoForSingleProducerAndConsumer) {
    constexpr int item_count = 512;
    BoundedQueue<int> queue{static_cast<std::size_t>(item_count)};
    std::latch start{1};
    std::vector<int> received;
    received.reserve(item_count);

    std::jthread producer([&] {
        start.wait();
        for (int value = 0; value < item_count; ++value) {
            if (!queue.try_push(value)) {
                return;
            }
        }
        queue.close();
    });
    std::jthread consumer([&] {
        start.wait();
        while (const std::optional<int> value = queue.wait_pop()) {
            received.push_back(*value);
        }
    });

    start.count_down();
    producer.join();
    consumer.join();

    ASSERT_EQ(received.size(), static_cast<std::size_t>(item_count));
    for (int value = 0; value < item_count; ++value) {
        EXPECT_EQ(received[static_cast<std::size_t>(value)], value);
    }
}

TEST(BoundedQueueTest, RejectsPushWhenFullAndAfterClose) {
    BoundedQueue<int> queue{2};
    EXPECT_TRUE(queue.try_push(1));
    EXPECT_TRUE(queue.try_push(2));
    EXPECT_FALSE(queue.try_push(3));
    EXPECT_EQ(queue.size(), 2U);
    EXPECT_EQ(queue.capacity(), 2U);

    queue.close();
    queue.close();
    EXPECT_TRUE(queue.closed());
    EXPECT_FALSE(queue.try_push(4));
}

TEST(BoundedQueueTest, CloseWakesBlockedConsumerAndQueuedValuesRemainDrainable) {
    BoundedQueue<int> queue{2};
    std::latch consumer_started{1};
    std::promise<bool> terminal_result;
    std::future<bool> terminal_future = terminal_result.get_future();
    std::jthread consumer([&] {
        consumer_started.count_down();
        terminal_result.set_value(!queue.wait_pop().has_value());
    });

    consumer_started.wait();
    queue.close();
    ASSERT_EQ(terminal_future.wait_for(1s), std::future_status::ready);
    EXPECT_TRUE(terminal_future.get());
    consumer.join();

    BoundedQueue<int> draining_queue{3};
    ASSERT_TRUE(draining_queue.try_push(10));
    ASSERT_TRUE(draining_queue.try_push(20));
    draining_queue.close();
    const std::optional<int> first = draining_queue.wait_pop();
    const std::optional<int> second = draining_queue.wait_pop();
    EXPECT_EQ(first, 10);
    EXPECT_EQ(second, 20);
    EXPECT_FALSE(draining_queue.wait_pop().has_value());
}

TEST(BoundedQueueTest, ConcurrentProducersTransferEveryUniqueValueExactlyOnce) {
    constexpr int producer_count = 4;
    constexpr int values_per_producer = 2500;
    constexpr int total_values = producer_count * values_per_producer;
    BoundedQueue<int> queue{61};
    std::latch start{1};
    std::atomic<bool> consumer_failed{false};
    std::vector<bool> seen(static_cast<std::size_t>(total_values), false);

    std::jthread consumer([&] {
        start.wait();
        int received = 0;
        while (received < total_values) {
            const std::optional<int> value = queue.wait_pop();
            if (!value.has_value() || *value < 0 || *value >= total_values ||
                seen[static_cast<std::size_t>(*value)]) {
                consumer_failed = true;
                return;
            }
            seen[static_cast<std::size_t>(*value)] = true;
            ++received;
        }
    });

    std::vector<std::jthread> producers;
    producers.reserve(producer_count);
    for (int producer = 0; producer < producer_count; ++producer) {
        producers.emplace_back([&, producer] {
            start.wait();
            const int first = producer * values_per_producer;
            for (int offset = 0; offset < values_per_producer; ++offset) {
                const int value = first + offset;
                while (!queue.try_push(value)) {
                    std::this_thread::yield();
                }
            }
        });
    }

    start.count_down();
    for (std::jthread& producer : producers) {
        producer.join();
    }
    queue.close();
    consumer.join();

    EXPECT_FALSE(consumer_failed.load());
    for (const bool was_seen : seen) {
        EXPECT_TRUE(was_seen);
    }
}

} // namespace
} // namespace matching::gateway_detail
