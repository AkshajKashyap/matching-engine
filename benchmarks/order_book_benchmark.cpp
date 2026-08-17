#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <vector>

#include <benchmark/benchmark.h>

#include "matching/order_book.hpp"
#include "workload.hpp"

namespace matching {
namespace {

[[nodiscard]] std::size_t size_from(const benchmark::State& state) {
    return static_cast<std::size_t>(state.range(0));
}

void require_accepted(const SubmitResult& result) {
    if (!result.accepted()) {
        std::abort();
    }
}

void require_cancelled(const CancelResult& result) {
    if (result.status != CancelStatus::Cancelled) {
        std::abort();
    }
}

void populate_same_price_orders(OrderBook& book, std::size_t count, Side side, std::uint64_t price) {
    for (std::size_t index = 0; index < count; ++index) {
        require_accepted(book.submit(NewLimitOrder{
            .id = OrderId{static_cast<std::uint64_t>(index) + 1U},
            .side = side,
            .limit_price = Price{price},
            .quantity = Quantity{1},
        }));
    }
}

void populate_price_levels(OrderBook& book, std::size_t count, Side side, std::uint64_t first_price) {
    for (std::size_t index = 0; index < count; ++index) {
        require_accepted(book.submit(NewLimitOrder{
            .id = OrderId{static_cast<std::uint64_t>(index) + 1U},
            .side = side,
            .limit_price = Price{first_price + static_cast<std::uint64_t>(index)},
            .quantity = Quantity{1},
        }));
    }
}

void BM_PassiveInsertUniqueLevels(benchmark::State& state) {
    const std::size_t order_count = size_from(state);
    for ([[maybe_unused]] auto iteration : state) {
        state.PauseTiming();
        auto book = std::make_unique<OrderBook>();
        state.ResumeTiming();

        populate_price_levels(*book, order_count, Side::Buy, 1'000);
        benchmark::DoNotOptimize(book->active_order_count());

        state.PauseTiming();
        book.reset();
        state.ResumeTiming();
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(order_count));
    state.counters["orders/s"] = benchmark::Counter(
        static_cast<double>(order_count), benchmark::Counter::kIsIterationInvariantRate);
}

void BM_PassiveInsertSameLevel(benchmark::State& state) {
    const std::size_t order_count = size_from(state);
    for ([[maybe_unused]] auto iteration : state) {
        state.PauseTiming();
        auto book = std::make_unique<OrderBook>();
        state.ResumeTiming();

        populate_same_price_orders(*book, order_count, Side::Buy, 100);
        benchmark::DoNotOptimize(book->active_order_count());

        state.PauseTiming();
        book.reset();
        state.ResumeTiming();
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(order_count));
    state.counters["orders/s"] = benchmark::Counter(
        static_cast<double>(order_count), benchmark::Counter::kIsIterationInvariantRate);
}

void BM_PassiveInsertExistingLevel(benchmark::State& state) {
    const std::size_t existing_order_count = size_from(state);
    for ([[maybe_unused]] auto iteration : state) {
        state.PauseTiming();
        auto book = std::make_unique<OrderBook>();
        populate_same_price_orders(*book, existing_order_count, Side::Buy, 100);
        state.ResumeTiming();

        const SubmitResult result = book->submit(NewLimitOrder{
            .id = OrderId{static_cast<std::uint64_t>(existing_order_count) + 1U},
            .side = Side::Buy,
            .limit_price = Price{100},
            .quantity = Quantity{1},
        });
        require_accepted(result);
        std::uint64_t resting_quantity = result.resting_quantity.units;
        benchmark::DoNotOptimize(resting_quantity);

        state.PauseTiming();
        book.reset();
        state.ResumeTiming();
    }
    state.counters["orders/s"] = benchmark::Counter(
        1.0, benchmark::Counter::kIsIterationInvariantRate);
}

void BM_PassiveInsertNewLevel(benchmark::State& state) {
    const std::size_t existing_level_count = size_from(state);
    constexpr std::uint64_t first_price = 1'000;
    for ([[maybe_unused]] auto iteration : state) {
        state.PauseTiming();
        auto book = std::make_unique<OrderBook>();
        populate_price_levels(*book, existing_level_count, Side::Buy, first_price);
        state.ResumeTiming();

        const SubmitResult result = book->submit(NewLimitOrder{
            .id = OrderId{static_cast<std::uint64_t>(existing_level_count) + 1U},
            .side = Side::Buy,
            .limit_price = Price{first_price + static_cast<std::uint64_t>(existing_level_count)},
            .quantity = Quantity{1},
        });
        require_accepted(result);
        std::uint64_t resting_quantity = result.resting_quantity.units;
        benchmark::DoNotOptimize(resting_quantity);

        state.PauseTiming();
        book.reset();
        state.ResumeTiming();
    }
    state.counters["orders/s"] = benchmark::Counter(
        1.0, benchmark::Counter::kIsIterationInvariantRate);
}

void BM_CancelKeepLevel(benchmark::State& state) {
    const std::size_t order_count = size_from(state);
    const OrderId target{static_cast<std::uint64_t>(order_count / 2U) + 1U};
    for ([[maybe_unused]] auto iteration : state) {
        state.PauseTiming();
        auto book = std::make_unique<OrderBook>();
        populate_same_price_orders(*book, order_count, Side::Buy, 100);
        state.ResumeTiming();

        CancelResult result = book->cancel(target);
        require_cancelled(result);
        benchmark::DoNotOptimize(result.cancelled_quantity.units);

        state.PauseTiming();
        book.reset();
        state.ResumeTiming();
    }
    state.counters["cancellations/s"] = benchmark::Counter(
        1.0, benchmark::Counter::kIsIterationInvariantRate);
}

void BM_CancelRemoveLevel(benchmark::State& state) {
    const std::size_t order_count = size_from(state);
    const OrderId target{static_cast<std::uint64_t>(order_count / 2U) + 1U};
    for ([[maybe_unused]] auto iteration : state) {
        state.PauseTiming();
        auto book = std::make_unique<OrderBook>();
        populate_price_levels(*book, order_count, Side::Buy, 1'000);
        state.ResumeTiming();

        CancelResult result = book->cancel(target);
        require_cancelled(result);
        benchmark::DoNotOptimize(result.cancelled_quantity.units);

        state.PauseTiming();
        book.reset();
        state.ResumeTiming();
    }
    state.counters["cancellations/s"] = benchmark::Counter(
        1.0, benchmark::Counter::kIsIterationInvariantRate);
}

void BM_MatchOneAsk(benchmark::State& state) {
    for ([[maybe_unused]] auto iteration : state) {
        state.PauseTiming();
        auto book = std::make_unique<OrderBook>();
        require_accepted(book->submit(NewLimitOrder{
            .id = OrderId{1}, .side = Side::Sell, .limit_price = Price{100}, .quantity = Quantity{1}}));
        state.ResumeTiming();

        const SubmitResult result = book->submit(NewLimitOrder{
            .id = OrderId{2}, .side = Side::Buy, .limit_price = Price{100}, .quantity = Quantity{1}});
        require_accepted(result);
        benchmark::DoNotOptimize(result.trades.size());

        state.PauseTiming();
        book.reset();
        state.ResumeTiming();
    }
    state.counters["matches/s"] = benchmark::Counter(
        1.0, benchmark::Counter::kIsIterationInvariantRate);
    state.counters["trades/s"] = benchmark::Counter(
        1.0, benchmark::Counter::kIsIterationInvariantRate);
}

void BM_MatchOneBid(benchmark::State& state) {
    for ([[maybe_unused]] auto iteration : state) {
        state.PauseTiming();
        auto book = std::make_unique<OrderBook>();
        require_accepted(book->submit(NewLimitOrder{
            .id = OrderId{1}, .side = Side::Buy, .limit_price = Price{100}, .quantity = Quantity{1}}));
        state.ResumeTiming();

        const SubmitResult result = book->submit(NewLimitOrder{
            .id = OrderId{2}, .side = Side::Sell, .limit_price = Price{100}, .quantity = Quantity{1}});
        require_accepted(result);
        benchmark::DoNotOptimize(result.trades.size());

        state.PauseTiming();
        book.reset();
        state.ResumeTiming();
    }
    state.counters["matches/s"] = benchmark::Counter(
        1.0, benchmark::Counter::kIsIterationInvariantRate);
    state.counters["trades/s"] = benchmark::Counter(
        1.0, benchmark::Counter::kIsIterationInvariantRate);
}

void BM_SweepSameLevel(benchmark::State& state) {
    const std::size_t maker_count = size_from(state);
    for ([[maybe_unused]] auto iteration : state) {
        state.PauseTiming();
        auto book = std::make_unique<OrderBook>();
        populate_same_price_orders(*book, maker_count, Side::Sell, 100);
        state.ResumeTiming();

        const SubmitResult result = book->submit(NewLimitOrder{
            .id = OrderId{static_cast<std::uint64_t>(maker_count) + 1U},
            .side = Side::Buy,
            .limit_price = Price{100},
            .quantity = Quantity{static_cast<std::uint64_t>(maker_count)},
        });
        require_accepted(result);
        benchmark::DoNotOptimize(result.trades.size());

        state.PauseTiming();
        book.reset();
        state.ResumeTiming();
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(maker_count));
    state.counters["makers/s"] = benchmark::Counter(
        static_cast<double>(maker_count), benchmark::Counter::kIsIterationInvariantRate);
}

void BM_SweepMultipleLevels(benchmark::State& state) {
    const std::size_t level_count = size_from(state);
    const std::uint64_t first_price = 1'000;
    for ([[maybe_unused]] auto iteration : state) {
        state.PauseTiming();
        auto book = std::make_unique<OrderBook>();
        populate_price_levels(*book, level_count, Side::Sell, first_price);
        state.ResumeTiming();

        const SubmitResult result = book->submit(NewLimitOrder{
            .id = OrderId{static_cast<std::uint64_t>(level_count) + 1U},
            .side = Side::Buy,
            .limit_price = Price{first_price + static_cast<std::uint64_t>(level_count) - 1U},
            .quantity = Quantity{static_cast<std::uint64_t>(level_count)},
        });
        require_accepted(result);
        benchmark::DoNotOptimize(result.trades.size());

        state.PauseTiming();
        book.reset();
        state.ResumeTiming();
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(level_count));
    state.counters["makers/s"] = benchmark::Counter(
        static_cast<double>(level_count), benchmark::Counter::kIsIterationInvariantRate);
}

void BM_MixedReplay(benchmark::State& state) {
    const std::vector<benchmark_support::Command> commands =
        benchmark_support::make_mixed_workload(size_from(state));
    std::uint64_t total_trades = 0;

    for ([[maybe_unused]] auto iteration : state) {
        state.PauseTiming();
        auto book = std::make_unique<OrderBook>();
        state.ResumeTiming();

        benchmark_support::ReplayStats stats = benchmark_support::replay(*book, commands);
        total_trades += stats.trades;
        benchmark::DoNotOptimize(stats.checksum);

        state.PauseTiming();
        book.reset();
        state.ResumeTiming();
    }
    state.SetItemsProcessed(
        state.iterations() * static_cast<std::int64_t>(commands.size()));
    state.counters["operations/s"] = benchmark::Counter(
        static_cast<double>(commands.size()), benchmark::Counter::kIsIterationInvariantRate);
    state.counters["trades/s"] = benchmark::Counter(
        static_cast<double>(total_trades), benchmark::Counter::kIsRate);
}

BENCHMARK(BM_PassiveInsertUniqueLevels)->Arg(10)->Arg(100)->Arg(1'000);
BENCHMARK(BM_PassiveInsertSameLevel)->Arg(10)->Arg(100)->Arg(1'000);
BENCHMARK(BM_PassiveInsertExistingLevel)->Arg(10)->Arg(100)->Arg(1'000);
BENCHMARK(BM_PassiveInsertNewLevel)->Arg(10)->Arg(100)->Arg(1'000);
BENCHMARK(BM_CancelKeepLevel)->Arg(10)->Arg(100)->Arg(1'000);
BENCHMARK(BM_CancelRemoveLevel)->Arg(10)->Arg(100)->Arg(1'000);
BENCHMARK(BM_MatchOneAsk);
BENCHMARK(BM_MatchOneBid);
BENCHMARK(BM_SweepSameLevel)->Arg(10)->Arg(100)->Arg(1'000);
BENCHMARK(BM_SweepMultipleLevels)->Arg(10)->Arg(100)->Arg(1'000);
BENCHMARK(BM_MixedReplay)->Arg(32)->Arg(256)->Arg(1'024);

} // namespace
} // namespace matching

BENCHMARK_MAIN();
