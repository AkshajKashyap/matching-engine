#include <cstdint>
#include <limits>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "matching/order_book.hpp"
#include "order_book_test_access.hpp"
#include "reference_order_book.hpp"

namespace matching {
namespace {

using test_support::ReferenceOrderBook;

struct Command {
    enum class Type { Submit, Cancel };

    Type type;
    NewLimitOrder order{
        .id = OrderId{0},
        .side = Side::Buy,
        .limit_price = Price{0},
        .quantity = Quantity{0},
    };
    OrderId cancel_id{};

    [[nodiscard]] static Command submit(std::uint64_t id, Side side, std::uint64_t price, std::uint64_t quantity) {
        return Command{
            .type = Type::Submit,
            .order = NewLimitOrder{
                .id = OrderId{id},
                .side = side,
                .limit_price = Price{price},
                .quantity = Quantity{quantity},
            },
        };
    }

    [[nodiscard]] static Command cancel(std::uint64_t id) {
        return Command{.type = Type::Cancel, .cancel_id = OrderId{id}};
    }

    [[nodiscard]] std::string describe() const {
        std::ostringstream output;
        if (type == Type::Submit) {
            output << "submit id=" << order.id.value << " side="
                   << (order.side == Side::Buy ? "buy" : "sell")
                   << " price=" << order.limit_price.ticks << " quantity=" << order.quantity.units;
        } else {
            output << "cancel id=" << cancel_id.value;
        }
        return output.str();
    }
};

[[nodiscard]] bool same_trade(const Trade& left, const Trade& right) {
    return left.id == right.id && left.maker_order_id == right.maker_order_id &&
           left.taker_order_id == right.taker_order_id && left.execution_price == right.execution_price &&
           left.execution_quantity == right.execution_quantity;
}

[[nodiscard]] bool same_submit_result(const SubmitResult& left, const SubmitResult& right) {
    if (left.status != right.status || left.rejection_reason != right.rejection_reason ||
        left.executed_quantity != right.executed_quantity || left.resting_quantity != right.resting_quantity ||
        left.trades.size() != right.trades.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.trades.size(); ++index) {
        if (!same_trade(left.trades[index], right.trades[index])) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool same_cancel_result(const CancelResult& left, const CancelResult& right) {
    return left.status == right.status && left.cancelled_quantity == right.cancelled_quantity;
}

[[nodiscard]] std::string describe_submit_result(const SubmitResult& result) {
    std::ostringstream output;
    output << "status=" << (result.accepted() ? "accepted" : "rejected") << " rejection=";
    if (result.rejection_reason.has_value()) {
        output << static_cast<unsigned int>(*result.rejection_reason);
    } else {
        output << "none";
    }
    output << " executed=" << result.executed_quantity.units
           << " resting=" << result.resting_quantity.units << " trades=[";
    for (std::size_t index = 0; index < result.trades.size(); ++index) {
        if (index != 0) {
            output << ", ";
        }
        const Trade& trade = result.trades[index];
        output << '(' << trade.id.value << ':' << trade.maker_order_id.value << ':'
               << trade.taker_order_id.value << ':' << trade.execution_price.ticks << ':'
               << trade.execution_quantity.units << ')';
    }
    output << ']';
    return output.str();
}

[[nodiscard]] std::string describe_cancel_result(const CancelResult& result) {
    std::ostringstream output;
    output << "status=" << (result.status == CancelStatus::Cancelled ? "cancelled" : "not-found")
           << " quantity=" << result.cancelled_quantity.units;
    return output.str();
}

[[nodiscard]] bool same_quote(const std::optional<Quote>& left, const std::optional<Quote>& right) {
    if (left.has_value() != right.has_value()) {
        return false;
    }
    return !left.has_value() || (left->price == right->price && left->quantity == right->quantity);
}

[[nodiscard]] bool same_order_view(const std::optional<OrderView>& left, const std::optional<OrderView>& right) {
    if (left.has_value() != right.has_value()) {
        return false;
    }
    return !left.has_value() ||
           (left->id == right->id && left->side == right->side &&
            left->limit_price == right->limit_price &&
            left->original_quantity == right->original_quantity &&
            left->remaining_quantity == right->remaining_quantity);
}

[[nodiscard]] bool same_depth(
    const std::vector<PriceLevelView>& left,
    const std::vector<PriceLevelView>& right) {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (left[index].price != right[index].price ||
            left[index].total_quantity != right[index].total_quantity ||
            left[index].order_count != right[index].order_count) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::string describe_depth(const std::vector<PriceLevelView>& levels) {
    std::ostringstream output;
    output << '[';
    for (std::size_t index = 0; index < levels.size(); ++index) {
        if (index != 0) {
            output << ", ";
        }
        output << '(' << levels[index].price.ticks << ':' << levels[index].total_quantity.units
               << ':' << levels[index].order_count << ')';
    }
    output << ']';
    return output.str();
}

class TraceRunner {
public:
    [[nodiscard]] std::string execute(const Command& command) {
        history_.push_back(command);
        remember(command.type == Command::Type::Submit ? command.order.id.value : command.cancel_id.value);

        if (command.type == Command::Type::Submit) {
            const SubmitResult production_result = production_.submit(command.order);
            const SubmitResult reference_result = reference_.submit(command.order);
            if (!same_submit_result(production_result, reference_result)) {
                last_result_comparison_ = "production submit: " + describe_submit_result(production_result) +
                                          "\nreference submit: " + describe_submit_result(reference_result);
                return "submit result mismatch";
            }
            if (production_result.accepted()) {
                accepted_ids_.push_back(command.order.id.value);
            }
            if (const std::string accounting_error = check_submission_accounting(command.order, production_result);
                !accounting_error.empty()) {
                return accounting_error;
            }
        } else {
            const CancelResult production_result = production_.cancel(command.cancel_id);
            const CancelResult reference_result = reference_.cancel(command.cancel_id);
            if (!same_cancel_result(production_result, reference_result)) {
                last_result_comparison_ = "production cancel: " + describe_cancel_result(production_result) +
                                          "\nreference cancel: " + describe_cancel_result(reference_result);
                return "cancel result mismatch";
            }
        }
        return compare_state();
    }

    [[nodiscard]] const std::vector<std::uint64_t>& accepted_ids() const noexcept {
        return accepted_ids_;
    }

    [[nodiscard]] std::vector<std::uint64_t> active_ids() const {
        std::vector<std::uint64_t> result;
        for (const std::uint64_t id : accepted_ids_) {
            if (production_.find(OrderId{id}).has_value()) {
                result.push_back(id);
            }
        }
        return result;
    }

    [[nodiscard]] std::string diagnostic(std::uint64_t seed, std::size_t operation, const std::string& issue) const {
        std::ostringstream output;
        output << "seed=" << seed << " operation=" << operation << " issue=" << issue << "\ntrace:";
        for (std::size_t index = 0; index < history_.size(); ++index) {
            output << "\n  " << index << ": " << history_[index].describe();
        }
        output << "\nproduction bids="
               << describe_depth(production_.depth(Side::Buy, std::numeric_limits<std::size_t>::max()))
               << " asks="
               << describe_depth(production_.depth(Side::Sell, std::numeric_limits<std::size_t>::max()))
               << "\nreference bids=" << describe_depth(reference_.depth(Side::Buy))
               << " asks=" << describe_depth(reference_.depth(Side::Sell));
        if (!last_result_comparison_.empty()) {
            output << "\n" << last_result_comparison_;
        }
        return output.str();
    }

private:
    OrderBook production_;
    ReferenceOrderBook reference_;
    std::vector<Command> history_;
    std::vector<std::uint64_t> known_ids_;
    std::unordered_set<std::uint64_t> known_id_set_;
    std::vector<std::uint64_t> accepted_ids_;
    std::uint64_t expected_next_trade_id_{1};
    std::string last_result_comparison_;

    void remember(std::uint64_t id) {
        if (known_id_set_.insert(id).second) {
            known_ids_.push_back(id);
        }
    }

    [[nodiscard]] std::string check_submission_accounting(
        const NewLimitOrder& submitted,
        const SubmitResult& result) {
        if (!result.accepted()) {
            return {};
        }
        std::uint64_t trade_quantity = 0;
        for (const Trade& trade : result.trades) {
            if (trade.id.value != expected_next_trade_id_ || trade.id.value == 0 ||
                trade.maker_order_id.value == 0 || trade.taker_order_id.value == 0 ||
                trade.execution_price.ticks == 0 || trade.execution_quantity.units == 0 ||
                trade_quantity > std::numeric_limits<std::uint64_t>::max() - trade.execution_quantity.units) {
                return "invalid or non-monotonic trade emitted";
            }
            trade_quantity += trade.execution_quantity.units;
            ++expected_next_trade_id_;
        }
        if (result.executed_quantity.units != trade_quantity ||
            result.executed_quantity.units > submitted.quantity.units ||
            result.resting_quantity.units > submitted.quantity.units - result.executed_quantity.units ||
            result.executed_quantity.units + result.resting_quantity.units != submitted.quantity.units) {
            return "submission quantity conservation failed";
        }
        return {};
    }

    [[nodiscard]] std::string compare_state() const {
        if (!testing::OrderBookTestAccess::invariants_hold(production_)) {
            return "production invariant check failed";
        }
        if (!same_quote(production_.best_bid(), reference_.best_bid()) ||
            !same_quote(production_.best_ask(), reference_.best_ask())) {
            return "best quote mismatch";
        }
        if (!same_depth(
                production_.depth(Side::Buy, std::numeric_limits<std::size_t>::max()),
                reference_.depth(Side::Buy)) ||
            !same_depth(
                production_.depth(Side::Sell, std::numeric_limits<std::size_t>::max()),
                reference_.depth(Side::Sell))) {
            return "depth mismatch";
        }
        if (production_.active_order_count() != reference_.active_order_count()) {
            return "active order count mismatch";
        }
        for (const std::uint64_t id : known_ids_) {
            if (!same_order_view(production_.find(OrderId{id}), reference_.find(OrderId{id}))) {
                return "find mismatch for id=" + std::to_string(id);
            }
        }
        return {};
    }
};

void assert_trace_command(
    TraceRunner& runner,
    std::uint64_t seed,
    std::size_t operation,
    const Command& command) {
    const std::string issue = runner.execute(command);
    ASSERT_TRUE(issue.empty()) << runner.diagnostic(seed, operation, issue);
}

[[nodiscard]] Command random_valid_submit(std::mt19937_64& random, std::uint64_t id) {
    const Side side = (random() & 1U) == 0U ? Side::Buy : Side::Sell;
    const std::uint64_t price = 95 + (random() % 11U);
    const std::uint64_t quantity = 1 + (random() % 8U);
    return Command::submit(id, side, price, quantity);
}

void run_random_trace(std::uint64_t seed, std::size_t command_count) {
    std::mt19937_64 random(seed);
    TraceRunner runner;
    std::uint64_t next_id = 1;

    for (std::size_t operation = 0; operation < command_count; ++operation) {
        const std::uint64_t choice = random() % 100U;
        Command command = Command::submit(next_id++, Side::Buy, 100, 1);

        if (choice < 65U) {
            command = random_valid_submit(random, next_id - 1U);
        } else if (choice < 78U) {
            const std::vector<std::uint64_t> active_ids = runner.active_ids();
            command = active_ids.empty()
                          ? Command::cancel(1'000'000'000U + static_cast<std::uint64_t>(operation))
                          : Command::cancel(active_ids[random() % active_ids.size()]);
        } else if (choice < 85U && !runner.accepted_ids().empty()) {
            command = Command::submit(
                runner.accepted_ids()[random() % runner.accepted_ids().size()],
                (random() & 1U) == 0U ? Side::Buy : Side::Sell,
                95 + (random() % 11U),
                1 + (random() % 8U));
        } else if (choice < 91U) {
            command = Command::submit(next_id - 1U, Side::Buy, 0, 1 + (random() % 8U));
        } else if (choice < 97U) {
            command = Command::submit(next_id - 1U, Side::Sell, 95 + (random() % 11U), 0);
        } else {
            command = Command::submit(0, Side::Buy, 95 + (random() % 11U), 1);
        }

        assert_trace_command(runner, seed, operation, command);
    }
}

TEST(OrderBookDifferentialTest, RandomizedFixedSeedsMatchIndependentReferenceModel) {
    constexpr std::uint64_t seeds[] = {
        0x0000'0000'00C0'FFEEULL,
        0x0000'0000'1234'5678ULL,
        0x0000'0000'DEAD'BEEFULL,
        0xA5A5'A5A5'5A5A'5A5AULL,
        0x0123'4567'89AB'CDEFULL,
    };

    for (const std::uint64_t seed : seeds) {
        run_random_trace(seed, 2'000);
    }
}

TEST(OrderBookDifferentialTest, LongAdversarialPublicApiTraceMatchesReferenceModel) {
    TraceRunner runner;
    constexpr std::uint64_t seed = 0xAD5E'5A71'A100'0001ULL;
    std::size_t operation = 0;

    for (std::uint64_t id = 1; id <= 120; ++id) {
        assert_trace_command(
            runner,
            seed,
            operation++,
            Command::submit(id, Side::Sell, 100 + ((id - 1U) % 5U), 1 + (id % 7U)));
    }
    assert_trace_command(runner, seed, operation++, Command::submit(1'000, Side::Buy, 104, 120));

    for (const std::uint64_t id : {12U, 37U, 64U, 91U, 118U}) {
        assert_trace_command(runner, seed, operation++, Command::cancel(id));
    }

    for (std::uint64_t id = 2'000; id < 2'080; ++id) {
        assert_trace_command(
            runner,
            seed,
            operation++,
            Command::submit(id, Side::Buy, 95 + ((id - 2'000U) % 5U), 1 + (id % 5U)));
    }
    assert_trace_command(runner, seed, operation++, Command::submit(3'000, Side::Sell, 95, 140));

    for (std::uint64_t offset = 0; offset < 40; ++offset) {
        const std::uint64_t id = 4'000 + offset;
        assert_trace_command(
            runner,
            seed,
            operation++,
            Command::submit(id, Side::Sell, 106 + (offset % 3U), 1 + (offset % 6U)));
        assert_trace_command(runner, seed, operation++, Command::cancel(id));
    }
}

} // namespace
} // namespace matching
