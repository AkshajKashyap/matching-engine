#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <vector>

#include "matching/order_book.hpp"

namespace matching::benchmark_support {

enum class CommandType { Submit, Cancel };

struct Command {
    CommandType type;
    NewLimitOrder order{
        .id = OrderId{0},
        .side = Side::Buy,
        .limit_price = Price{0},
        .quantity = Quantity{0},
    };
    OrderId cancel_id{0};
};

struct ReplayStats {
    std::uint64_t operations{};
    std::uint64_t trades{};
    std::uint64_t checksum{};
};

struct WorkloadCoverage {
    std::uint64_t passive_submissions{};
    std::uint64_t crossing_submissions{};
    std::uint64_t full_fills{};
    std::uint64_t partial_fills{};
    std::uint64_t same_level_matches{};
    std::uint64_t multi_level_matches{};
    std::uint64_t cancellations{};
};

[[nodiscard]] inline std::vector<Command> make_mixed_workload(std::size_t blocks) {
    std::vector<Command> commands;
    commands.reserve(blocks * 4U);
    std::uint64_t next_order_id = 1;

    for (std::size_t block = 0; block < blocks; ++block) {
        const std::uint64_t offset = static_cast<std::uint64_t>(block);
        const std::uint64_t traded_price = 100 + (offset % 5U);
        const std::uint64_t sell_quantity = 2 + (offset % 3U);
        const std::uint64_t buy_quantity = 1 + (offset % 4U);
        const std::uint64_t passive_bid_id = next_order_id + 2U;

        commands.push_back(Command{
            .type = CommandType::Submit,
            .order = NewLimitOrder{
                .id = OrderId{next_order_id++},
                .side = Side::Sell,
                .limit_price = Price{traded_price},
                .quantity = Quantity{sell_quantity},
            },
        });
        commands.push_back(Command{
            .type = CommandType::Submit,
            .order = NewLimitOrder{
                .id = OrderId{next_order_id++},
                .side = Side::Buy,
                .limit_price = Price{traded_price},
                .quantity = Quantity{buy_quantity},
            },
        });
        commands.push_back(Command{
            .type = CommandType::Submit,
            .order = NewLimitOrder{
                .id = OrderId{next_order_id++},
                .side = Side::Buy,
                .limit_price = Price{80 + (offset % 5U)},
                .quantity = Quantity{1 + (offset % 5U)},
            },
        });
        commands.push_back(Command{
            .type = CommandType::Cancel,
            .cancel_id = OrderId{passive_bid_id},
        });
    }

    return commands;
}

[[nodiscard]] inline ReplayStats replay(OrderBook& book, const std::vector<Command>& commands) {
    ReplayStats stats{};
    for (const Command& command : commands) {
        if (command.type == CommandType::Submit) {
            const SubmitResult result = book.submit(command.order);
            if (!result.accepted()) {
                std::abort();
            }
            stats.checksum += result.executed_quantity.units + result.resting_quantity.units;
            stats.trades += static_cast<std::uint64_t>(result.trades.size());
            for (const Trade& trade : result.trades) {
                stats.checksum ^= trade.id.value ^ trade.maker_order_id.value ^ trade.taker_order_id.value ^
                                  trade.execution_price.ticks ^ trade.execution_quantity.units;
            }
        } else {
            const CancelResult result = book.cancel(command.cancel_id);
            if (result.status != CancelStatus::Cancelled) {
                std::abort();
            }
            stats.checksum += result.cancelled_quantity.units;
        }
        ++stats.operations;
    }
    return stats;
}

[[nodiscard]] inline WorkloadCoverage analyze_coverage(const std::vector<Command>& commands) {
    OrderBook book;
    WorkloadCoverage coverage{};

    for (const Command& command : commands) {
        if (command.type == CommandType::Cancel) {
            const CancelResult result = book.cancel(command.cancel_id);
            if (result.status != CancelStatus::Cancelled) {
                std::abort();
            }
            ++coverage.cancellations;
            continue;
        }

        const SubmitResult result = book.submit(command.order);
        if (!result.accepted()) {
            std::abort();
        }
        if (result.trades.empty()) {
            ++coverage.passive_submissions;
            continue;
        }

        ++coverage.crossing_submissions;
        if (result.resting_quantity.units == 0) {
            ++coverage.full_fills;
        } else {
            ++coverage.partial_fills;
        }

        const Price first_execution_price = result.trades.front().execution_price;
        const bool crosses_multiple_levels = std::any_of(
            result.trades.begin(),
            result.trades.end(),
            [first_execution_price](const Trade& trade) {
                return trade.execution_price != first_execution_price;
            });
        if (crosses_multiple_levels) {
            ++coverage.multi_level_matches;
        } else {
            ++coverage.same_level_matches;
        }
    }

    return coverage;
}

[[nodiscard]] inline bool exercises_required_paths(const WorkloadCoverage& coverage) noexcept {
    return coverage.passive_submissions > 0 && coverage.crossing_submissions > 0 &&
           coverage.full_fills > 0 && coverage.partial_fills > 0 &&
           coverage.same_level_matches > 0 && coverage.multi_level_matches > 0 &&
           coverage.cancellations > 0;
}

} // namespace matching::benchmark_support
