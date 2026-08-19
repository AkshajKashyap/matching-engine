#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <benchmark/benchmark.h>

#include "matching/durable_engine.hpp"
#include "matching/order_book.hpp"

#include <fcntl.h>
#include <unistd.h>

namespace matching {
namespace {

class TemporaryJournal {
public:
    TemporaryJournal() {
        std::array<char, 64> template_path{};
        constexpr char kPattern[] = "/tmp/matching-engine-benchmark-XXXXXX";
        std::copy(std::begin(kPattern), std::end(kPattern), template_path.begin());
        char* directory = ::mkdtemp(template_path.data());
        if (directory == nullptr) {
            std::abort();
        }
        directory_ = directory;
        journal_path_ = directory_ / "journal.bin";
    }

    TemporaryJournal(const TemporaryJournal&) = delete;
    TemporaryJournal& operator=(const TemporaryJournal&) = delete;

    ~TemporaryJournal() {
        std::error_code error;
        std::filesystem::remove_all(directory_, error);
    }

    [[nodiscard]] const std::filesystem::path& journal_path() const noexcept {
        return journal_path_;
    }

private:
    std::filesystem::path directory_;
    std::filesystem::path journal_path_;
};

[[nodiscard]] NewLimitOrder order(
    std::uint64_t id,
    Side side,
    std::uint64_t price,
    std::uint64_t quantity) {
    return NewLimitOrder{
        .id = OrderId{id},
        .side = side,
        .limit_price = Price{price},
        .quantity = Quantity{quantity},
    };
}

[[nodiscard]] DurableEngine create_engine_or_abort(const std::filesystem::path& path) {
    auto result = DurableEngine::create(path);
    if (!std::holds_alternative<DurableEngine>(result)) {
        std::abort();
    }
    return std::move(std::get<DurableEngine>(result));
}

void require_submit(const DurableEngine::SubmitCommandResult& result) {
    if (!std::holds_alternative<SubmitResult>(result)) {
        std::abort();
    }
}

void require_cancel(const DurableEngine::CancelCommandResult& result) {
    if (!std::holds_alternative<CancelResult>(result)) {
        std::abort();
    }
}

void write_recovery_fixture_or_abort(const std::filesystem::path& path, std::size_t record_count) {
    std::vector<std::byte> bytes = encode_file_header();
    bytes.reserve(kJournalFileHeaderSize + record_count * (kJournalFrameHeaderSize + kSubmitLimitOrderPayloadSize));
    for (std::size_t index = 0; index < record_count; ++index) {
        const EncodedJournalRecord frame = encode_record(
            JournalSequence{static_cast<std::uint64_t>(index) + 1U},
            SubmitLimitOrderCommand{.order = order(
                static_cast<std::uint64_t>(index) + 1U,
                Side::Buy,
                100U + static_cast<std::uint64_t>(index % 10U),
                1)});
        if (!std::holds_alternative<std::vector<std::byte>>(frame)) {
            std::abort();
        }
        const std::vector<std::byte>& encoded = std::get<std::vector<std::byte>>(frame);
        bytes.insert(bytes.end(), encoded.begin(), encoded.end());
    }

    const std::string filename = path.string();
    const int descriptor = ::open(filename.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (descriptor < 0) {
        std::abort();
    }
    std::size_t written = 0;
    while (written < bytes.size()) {
        const ssize_t result = ::write(descriptor, bytes.data() + written, bytes.size() - written);
        if (result <= 0) {
            (void)::close(descriptor);
            std::abort();
        }
        written += static_cast<std::size_t>(result);
    }
    if (::close(descriptor) != 0) {
        std::abort();
    }
}

void BM_InMemoryPassiveSubmit(benchmark::State& state) {
    for ([[maybe_unused]] auto iteration : state) {
        state.PauseTiming();
        {
            OrderBook book;
            state.ResumeTiming();
            const SubmitResult result = book.submit(order(1, Side::Buy, 100, 1));
            if (!result.accepted()) {
                std::abort();
            }
            state.PauseTiming();
        }
        state.ResumeTiming();
    }
    state.SetItemsProcessed(state.iterations());
}

void BM_DurablePassiveSubmit(benchmark::State& state) {
    for ([[maybe_unused]] auto iteration : state) {
        state.PauseTiming();
        {
            TemporaryJournal journal;
            DurableEngine engine = create_engine_or_abort(journal.journal_path());
            state.ResumeTiming();
            const auto result = engine.submit(order(1, Side::Buy, 100, 1));
            require_submit(result);
            state.PauseTiming();
        }
        state.ResumeTiming();
    }
    state.SetItemsProcessed(state.iterations());
}

void BM_InMemoryAggressiveSubmit(benchmark::State& state) {
    for ([[maybe_unused]] auto iteration : state) {
        state.PauseTiming();
        {
            OrderBook book;
            if (!book.submit(order(1, Side::Sell, 100, 1)).accepted()) {
                std::abort();
            }
            state.ResumeTiming();
            const SubmitResult result = book.submit(order(2, Side::Buy, 100, 1));
            if (!result.accepted()) {
                std::abort();
            }
            state.PauseTiming();
        }
        state.ResumeTiming();
    }
    state.SetItemsProcessed(state.iterations());
}

void BM_DurableAggressiveSubmit(benchmark::State& state) {
    for ([[maybe_unused]] auto iteration : state) {
        state.PauseTiming();
        {
            TemporaryJournal journal;
            DurableEngine engine = create_engine_or_abort(journal.journal_path());
            require_submit(engine.submit(order(1, Side::Sell, 100, 1)));
            state.ResumeTiming();
            const auto result = engine.submit(order(2, Side::Buy, 100, 1));
            require_submit(result);
            state.PauseTiming();
        }
        state.ResumeTiming();
    }
    state.SetItemsProcessed(state.iterations());
}

void BM_InMemoryCancellation(benchmark::State& state) {
    for ([[maybe_unused]] auto iteration : state) {
        state.PauseTiming();
        {
            OrderBook book;
            if (!book.submit(order(1, Side::Buy, 100, 1)).accepted()) {
                std::abort();
            }
            state.ResumeTiming();
            const CancelResult result = book.cancel(OrderId{1});
            if (result.status != CancelStatus::Cancelled) {
                std::abort();
            }
            state.PauseTiming();
        }
        state.ResumeTiming();
    }
    state.SetItemsProcessed(state.iterations());
}

void BM_DurableCancellation(benchmark::State& state) {
    for ([[maybe_unused]] auto iteration : state) {
        state.PauseTiming();
        {
            TemporaryJournal journal;
            DurableEngine engine = create_engine_or_abort(journal.journal_path());
            require_submit(engine.submit(order(1, Side::Buy, 100, 1)));
            state.ResumeTiming();
            const auto result = engine.cancel(OrderId{1});
            require_cancel(result);
            state.PauseTiming();
        }
        state.ResumeTiming();
    }
    state.SetItemsProcessed(state.iterations());
}

void BM_InMemoryMixedStream(benchmark::State& state) {
    for ([[maybe_unused]] auto iteration : state) {
        state.PauseTiming();
        {
            OrderBook book;
            state.ResumeTiming();
            const SubmitResult first = book.submit(order(1, Side::Sell, 100, 2));
            const SubmitResult second = book.submit(order(2, Side::Buy, 100, 1));
            const SubmitResult third = book.submit(order(3, Side::Buy, 90, 1));
            const CancelResult fourth = book.cancel(OrderId{3});
            if (!first.accepted() || !second.accepted() || !third.accepted() ||
                fourth.status != CancelStatus::Cancelled) {
                std::abort();
            }
            state.PauseTiming();
        }
        state.ResumeTiming();
    }
    state.SetItemsProcessed(state.iterations() * 4);
}

void BM_DurableMixedStream(benchmark::State& state) {
    for ([[maybe_unused]] auto iteration : state) {
        state.PauseTiming();
        {
            TemporaryJournal journal;
            DurableEngine engine = create_engine_or_abort(journal.journal_path());
            state.ResumeTiming();
            require_submit(engine.submit(order(1, Side::Sell, 100, 2)));
            const auto second = engine.submit(order(2, Side::Buy, 100, 1));
            require_submit(second);
            require_submit(engine.submit(order(3, Side::Buy, 90, 1)));
            require_cancel(engine.cancel(OrderId{3}));
            state.PauseTiming();
        }
        state.ResumeTiming();
    }
    state.SetItemsProcessed(state.iterations() * 4);
}

void BM_DurableRecovery(benchmark::State& state) {
    const std::size_t record_count = static_cast<std::size_t>(state.range(0));
    TemporaryJournal journal;
    write_recovery_fixture_or_abort(journal.journal_path(), record_count);

    for ([[maybe_unused]] auto iteration : state) {
        {
            auto result = DurableEngine::recover(journal.journal_path());
            if (!std::holds_alternative<DurableEngine>(result)) {
                std::abort();
            }
            DurableEngine recovered = std::move(std::get<DurableEngine>(result));
            if (recovered.order_book().active_order_count() != record_count) {
                std::abort();
            }
            state.PauseTiming();
        }
        state.ResumeTiming();
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(record_count));
}

BENCHMARK(BM_InMemoryPassiveSubmit);
BENCHMARK(BM_DurablePassiveSubmit);
BENCHMARK(BM_InMemoryAggressiveSubmit);
BENCHMARK(BM_DurableAggressiveSubmit);
BENCHMARK(BM_InMemoryCancellation);
BENCHMARK(BM_DurableCancellation);
BENCHMARK(BM_InMemoryMixedStream);
BENCHMARK(BM_DurableMixedStream);
BENCHMARK(BM_DurableRecovery)->Arg(100)->Arg(1'000)->Arg(10'000);

} // namespace
} // namespace matching

BENCHMARK_MAIN();
