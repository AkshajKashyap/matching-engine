#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>

#include "matching/order_book.hpp"
#include "workload.hpp"

namespace {

[[nodiscard]] std::size_t replay_count_from_args(int argc, char* argv[]) {
    constexpr std::size_t default_replay_count = 4'000;
    if (argc == 1) {
        return default_replay_count;
    }
    if (argc != 2) {
        std::cerr << "usage: order_book_profile_workload [replay-count]\\n";
        std::exit(EXIT_FAILURE);
    }

    char* end = nullptr;
    errno = 0;
    const unsigned long long parsed = std::strtoull(argv[1], &end, 10);
    if (errno != 0 || end == argv[1] || *end != '\0' || parsed == 0 ||
        parsed > std::numeric_limits<std::size_t>::max()) {
        std::cerr << "replay-count must be a positive integer\\n";
        std::exit(EXIT_FAILURE);
    }
    return static_cast<std::size_t>(parsed);
}

} // namespace

int main(int argc, char* argv[]) {
    constexpr std::size_t blocks_per_replay = 256;
    const std::size_t replay_count = replay_count_from_args(argc, argv);
    const std::vector<matching::benchmark_support::Command> commands =
        matching::benchmark_support::make_mixed_workload(blocks_per_replay);
    const matching::benchmark_support::WorkloadCoverage coverage =
        matching::benchmark_support::analyze_coverage(commands);
    if (!matching::benchmark_support::exercises_required_paths(coverage)) {
        std::abort();
    }

    std::uint64_t checksum = 0;
    std::uint64_t trades = 0;
    for (std::size_t replay = 0; replay < replay_count; ++replay) {
        matching::OrderBook book;
        const matching::benchmark_support::ReplayStats stats =
            matching::benchmark_support::replay(book, commands);
        checksum ^= stats.checksum + static_cast<std::uint64_t>(replay);
        trades += stats.trades;
    }

    std::cout << "operations=" << (commands.size() * replay_count) << " trades=" << trades
              << " checksum=" << checksum << '\n';
    return 0;
}
