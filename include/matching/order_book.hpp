#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "matching/order.hpp"
#include "matching/trade.hpp"

namespace matching {

namespace testing {
struct OrderBookTestAccess;
}

enum class SubmissionStatus : std::uint8_t {
    Accepted,
    Rejected,
};

enum class RejectionReason : std::uint8_t {
    InvalidOrderId,
    InvalidSide,
    InvalidPrice,
    InvalidQuantity,
    DuplicateOrderId,
    QuantityOverflow,
    SequenceExhausted,
    TradeIdExhausted,
};

struct SubmitResult {
    SubmissionStatus status{SubmissionStatus::Rejected};
    std::optional<RejectionReason> rejection_reason;
    Quantity executed_quantity{};
    Quantity resting_quantity{};
    std::vector<Trade> trades;

    [[nodiscard]] constexpr bool accepted() const noexcept {
        return status == SubmissionStatus::Accepted;
    }
};

enum class CancelStatus : std::uint8_t {
    Cancelled,
    NotFound,
};

struct CancelResult {
    CancelStatus status{CancelStatus::NotFound};
    Quantity cancelled_quantity{};
};

struct Quote {
    Price price;
    Quantity quantity;
};

struct PriceLevelView {
    Price price;
    Quantity total_quantity;
    std::size_t order_count{};
};

// A single-instrument, synchronous, in-memory limit order book with price-time
// priority matching and cancellation.
class OrderBook {
public:
    OrderBook();
    ~OrderBook();

    OrderBook(const OrderBook&) = delete;
    OrderBook& operator=(const OrderBook&) = delete;
    OrderBook(OrderBook&&) = delete;
    OrderBook& operator=(OrderBook&&) = delete;

    [[nodiscard]] SubmitResult submit(NewLimitOrder order);
    [[nodiscard]] CancelResult cancel(OrderId id);

    [[nodiscard]] std::optional<Quote> best_bid() const;
    [[nodiscard]] std::optional<Quote> best_ask() const;
    [[nodiscard]] std::optional<OrderView> find(OrderId id) const;
    [[nodiscard]] std::vector<PriceLevelView> depth(Side side, std::size_t max_levels) const;
    [[nodiscard]] std::size_t active_order_count() const noexcept;

private:
    class Impl;

    [[nodiscard]] bool invariants_hold_for_testing() const;
    [[nodiscard]] std::vector<OrderId> order_ids_at_for_testing(Side side, Price price) const;
    void set_next_trade_id_for_testing(TradeId id);

    std::unique_ptr<Impl> impl_;

    friend struct testing::OrderBookTestAccess;
};

} // namespace matching
