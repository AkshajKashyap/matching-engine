#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <map>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "matching/order_book.hpp"

namespace matching::test_support {

// Deliberately simple, test-only oracle. It does not share production storage
// types or matching helpers: price levels use deque queues and cancellation
// scans the book linearly.
class ReferenceOrderBook {
public:
    [[nodiscard]] SubmitResult submit(NewLimitOrder incoming) {
        if (incoming.id.value == 0) {
            return reject(RejectionReason::InvalidOrderId);
        }
        if (incoming.side != Side::Buy && incoming.side != Side::Sell) {
            return reject(RejectionReason::InvalidSide);
        }
        if (incoming.limit_price.ticks == 0) {
            return reject(RejectionReason::InvalidPrice);
        }
        if (incoming.quantity.units == 0) {
            return reject(RejectionReason::InvalidQuantity);
        }
        if (seen_ids_.contains(incoming.id.value)) {
            return reject(RejectionReason::DuplicateOrderId);
        }

        const MatchPlan plan = incoming.side == Side::Buy
                                   ? preview(asks_, incoming.limit_price.ticks, incoming.quantity.units,
                                             [](std::uint64_t best, std::uint64_t limit) {
                                                 return best <= limit;
                                             })
                                   : preview(bids_, incoming.limit_price.ticks, incoming.quantity.units,
                                             [](std::uint64_t best, std::uint64_t limit) {
                                                 return best >= limit;
                                             });
        if (!can_issue_trade_ids(plan.trade_count)) {
            return reject(RejectionReason::TradeIdExhausted);
        }
        if (plan.remaining > 0 && next_arrival_sequence_ == 0) {
            return reject(RejectionReason::SequenceExhausted);
        }
        if (plan.remaining > 0 && would_overflow_at_resting_level(incoming, plan.remaining)) {
            return reject(RejectionReason::QuantityOverflow);
        }

        std::vector<Trade> trades;
        trades.reserve(plan.trade_count);
        seen_ids_.insert(incoming.id.value);

        std::uint64_t remaining = incoming.quantity.units;
        if (incoming.side == Side::Buy) {
            match_buy(incoming, remaining, trades);
        } else {
            match_sell(incoming, remaining, trades);
        }

        if (remaining > 0) {
            rest(incoming, remaining);
        }

        return SubmitResult{
            .status = SubmissionStatus::Accepted,
            .rejection_reason = std::nullopt,
            .executed_quantity = Quantity{incoming.quantity.units - remaining},
            .resting_quantity = Quantity{remaining},
            .trades = std::move(trades),
        };
    }

    [[nodiscard]] CancelResult cancel(OrderId id) {
        if (const auto cancelled = cancel_from(bids_, id.value); cancelled.has_value()) {
            return CancelResult{.status = CancelStatus::Cancelled, .cancelled_quantity = *cancelled};
        }
        if (const auto cancelled = cancel_from(asks_, id.value); cancelled.has_value()) {
            return CancelResult{.status = CancelStatus::Cancelled, .cancelled_quantity = *cancelled};
        }
        return CancelResult{.status = CancelStatus::NotFound, .cancelled_quantity = Quantity{0}};
    }

    [[nodiscard]] std::optional<Quote> best_bid() const {
        return best_from(bids_);
    }

    [[nodiscard]] std::optional<Quote> best_ask() const {
        return best_from(asks_);
    }

    [[nodiscard]] std::optional<OrderView> find(OrderId id) const {
        const auto active = active_orders_.find(id.value);
        if (active == active_orders_.end()) {
            return std::nullopt;
        }
        return active->second;
    }

    [[nodiscard]] std::vector<PriceLevelView> depth(Side side) const {
        if (side == Side::Buy) {
            return depth_from(bids_);
        }
        if (side == Side::Sell) {
            return depth_from(asks_);
        }
        return {};
    }

    [[nodiscard]] std::size_t active_order_count() const noexcept {
        return active_orders_.size();
    }

private:
    struct RestingOrder {
        std::uint64_t id;
        Side side;
        std::uint64_t price;
        std::uint64_t original_quantity;
        std::uint64_t remaining_quantity;
    };

    struct PriceLevel {
        std::deque<RestingOrder> orders;
        std::uint64_t total_quantity{};
    };

    struct MatchPlan {
        std::uint64_t remaining{};
        std::size_t trade_count{};
    };

    using BidLevels = std::map<std::uint64_t, PriceLevel, std::greater<>>;
    using AskLevels = std::map<std::uint64_t, PriceLevel, std::less<>>;

    BidLevels bids_;
    AskLevels asks_;
    std::unordered_map<std::uint64_t, OrderView> active_orders_;
    std::unordered_set<std::uint64_t> seen_ids_;
    std::uint64_t next_arrival_sequence_{1};
    std::uint64_t next_trade_id_{1};

    [[nodiscard]] static SubmitResult reject(RejectionReason reason) {
        return SubmitResult{
            .status = SubmissionStatus::Rejected,
            .rejection_reason = reason,
            .executed_quantity = Quantity{0},
            .resting_quantity = Quantity{0},
            .trades = {},
        };
    }

    [[nodiscard]] static bool would_overflow(std::uint64_t current, std::uint64_t addition) noexcept {
        return current > std::numeric_limits<std::uint64_t>::max() - addition;
    }

    template <typename Levels, typename Crosses>
    [[nodiscard]] static MatchPlan preview(
        const Levels& levels,
        std::uint64_t incoming_price,
        std::uint64_t incoming_quantity,
        Crosses crosses) {
        MatchPlan plan{.remaining = incoming_quantity};
        for (const auto& [price, level] : levels) {
            if (plan.remaining == 0 || !crosses(price, incoming_price)) {
                break;
            }
            for (const RestingOrder& maker : level.orders) {
                plan.remaining -= std::min(plan.remaining, maker.remaining_quantity);
                ++plan.trade_count;
                if (plan.remaining == 0) {
                    break;
                }
            }
        }
        return plan;
    }

    [[nodiscard]] bool can_issue_trade_ids(std::size_t trade_count) const noexcept {
        std::uint64_t candidate = next_trade_id_;
        for (std::size_t index = 0; index < trade_count; ++index) {
            if (candidate == 0) {
                return false;
            }
            ++candidate;
        }
        return true;
    }

    [[nodiscard]] bool would_overflow_at_resting_level(
        const NewLimitOrder& incoming,
        std::uint64_t remaining) const {
        if (incoming.side == Side::Buy) {
            const auto level = bids_.find(incoming.limit_price.ticks);
            return level != bids_.end() && would_overflow(level->second.total_quantity, remaining);
        }
        const auto level = asks_.find(incoming.limit_price.ticks);
        return level != asks_.end() && would_overflow(level->second.total_quantity, remaining);
    }

    template <typename Levels>
    void remove_filled_front(Levels& levels) {
        PriceLevel& level = levels.begin()->second;
        const std::uint64_t maker_id = level.orders.front().id;
        active_orders_.erase(maker_id);
        level.orders.pop_front();
        if (level.orders.empty()) {
            levels.erase(levels.begin());
        }
    }

    void match_buy(const NewLimitOrder& incoming, std::uint64_t& remaining, std::vector<Trade>& trades) {
        while (remaining > 0 && !asks_.empty() && asks_.begin()->first <= incoming.limit_price.ticks) {
            PriceLevel& level = asks_.begin()->second;
            RestingOrder& maker = level.orders.front();
            const std::uint64_t executed = std::min(remaining, maker.remaining_quantity);
            trades.push_back(Trade{
                .id = TradeId{next_trade_id_++},
                .maker_order_id = OrderId{maker.id},
                .taker_order_id = incoming.id,
                .execution_price = Price{maker.price},
                .execution_quantity = Quantity{executed},
            });
            remaining -= executed;
            maker.remaining_quantity -= executed;
            level.total_quantity -= executed;
            if (maker.remaining_quantity == 0) {
                remove_filled_front(asks_);
            } else {
                active_orders_.at(maker.id).remaining_quantity = Quantity{maker.remaining_quantity};
            }
        }
    }

    void match_sell(const NewLimitOrder& incoming, std::uint64_t& remaining, std::vector<Trade>& trades) {
        while (remaining > 0 && !bids_.empty() && bids_.begin()->first >= incoming.limit_price.ticks) {
            PriceLevel& level = bids_.begin()->second;
            RestingOrder& maker = level.orders.front();
            const std::uint64_t executed = std::min(remaining, maker.remaining_quantity);
            trades.push_back(Trade{
                .id = TradeId{next_trade_id_++},
                .maker_order_id = OrderId{maker.id},
                .taker_order_id = incoming.id,
                .execution_price = Price{maker.price},
                .execution_quantity = Quantity{executed},
            });
            remaining -= executed;
            maker.remaining_quantity -= executed;
            level.total_quantity -= executed;
            if (maker.remaining_quantity == 0) {
                remove_filled_front(bids_);
            } else {
                active_orders_.at(maker.id).remaining_quantity = Quantity{maker.remaining_quantity};
            }
        }
    }

    void rest(const NewLimitOrder& incoming, std::uint64_t remaining) {
        PriceLevel* level = nullptr;
        if (incoming.side == Side::Buy) {
            level = &bids_[incoming.limit_price.ticks];
        } else {
            level = &asks_[incoming.limit_price.ticks];
        }
        level->orders.push_back(RestingOrder{
            .id = incoming.id.value,
            .side = incoming.side,
            .price = incoming.limit_price.ticks,
            .original_quantity = incoming.quantity.units,
            .remaining_quantity = remaining,
        });
        level->total_quantity += remaining;
        active_orders_.emplace(
            incoming.id.value,
            OrderView{
                .id = incoming.id,
                .side = incoming.side,
                .limit_price = incoming.limit_price,
                .original_quantity = incoming.quantity,
                .remaining_quantity = Quantity{remaining},
            });
        ++next_arrival_sequence_;
    }

    template <typename Levels>
    [[nodiscard]] std::optional<Quantity> cancel_from(Levels& levels, std::uint64_t id) {
        for (auto level_it = levels.begin(); level_it != levels.end(); ++level_it) {
            PriceLevel& level = level_it->second;
            for (auto order_it = level.orders.begin(); order_it != level.orders.end(); ++order_it) {
                if (order_it->id != id) {
                    continue;
                }
                const Quantity cancelled{order_it->remaining_quantity};
                level.total_quantity -= cancelled.units;
                level.orders.erase(order_it);
                active_orders_.erase(id);
                if (level.orders.empty()) {
                    levels.erase(level_it);
                }
                return cancelled;
            }
        }
        return std::nullopt;
    }

    template <typename Levels>
    [[nodiscard]] static std::optional<Quote> best_from(const Levels& levels) {
        if (levels.empty()) {
            return std::nullopt;
        }
        return Quote{
            .price = Price{levels.begin()->first},
            .quantity = Quantity{levels.begin()->second.total_quantity},
        };
    }

    template <typename Levels>
    [[nodiscard]] static std::vector<PriceLevelView> depth_from(const Levels& levels) {
        std::vector<PriceLevelView> result;
        result.reserve(levels.size());
        for (const auto& [price, level] : levels) {
            result.push_back(PriceLevelView{
                .price = Price{price},
                .total_quantity = Quantity{level.total_quantity},
                .order_count = level.orders.size(),
            });
        }
        return result;
    }
};

} // namespace matching::test_support
