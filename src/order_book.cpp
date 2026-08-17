#include "matching/order_book.hpp"

#include <algorithm>
#include <cassert>
#include <exception>
#include <limits>
#include <list>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace matching {

namespace {

[[nodiscard]] bool is_valid_side(Side side) noexcept {
    return side == Side::Buy || side == Side::Sell;
}

[[nodiscard]] bool would_overflow(Quantity current, Quantity addition) noexcept {
    return current.units > std::numeric_limits<std::uint64_t>::max() - addition.units;
}

[[nodiscard]] SubmitResult rejected(RejectionReason reason) {
    return SubmitResult{
        .status = SubmissionStatus::Rejected,
        .rejection_reason = reason,
        .executed_quantity = Quantity{0},
        .resting_quantity = Quantity{0},
        .trades = {},
    };
}

} // namespace

class OrderBook::Impl {
public:
    struct RestingOrder {
        OrderId id;
        Side side;
        Price limit_price;
        Quantity original_quantity;
        Quantity remaining_quantity;
        std::uint64_t arrival_sequence;
    };

    struct PriceLevel {
        std::list<RestingOrder> orders;
        Quantity total_quantity{};
    };

    using BidLevels = std::map<Price, PriceLevel, std::greater<Price>>;
    using AskLevels = std::map<Price, PriceLevel, std::less<Price>>;

    struct OrderLocator {
        Side side;
        Price price;
        std::list<RestingOrder>::iterator order;
    };

    struct MatchPlan {
        Quantity remaining_quantity;
        std::size_t trade_count{};
    };

    BidLevels bids;
    AskLevels asks;
    std::unordered_map<OrderId, OrderLocator, OrderIdHash> active_orders;
    std::unordered_set<OrderId, OrderIdHash> seen_order_ids;
    std::uint64_t next_arrival_sequence{1};
    std::uint64_t next_trade_id{1};

    template <typename Levels>
    void insert_resting_order(
        Levels& levels,
        const NewLimitOrder& incoming,
        Quantity resting_quantity,
        std::uint64_t arrival_sequence) {
        const auto [level_it, inserted_level] = levels.try_emplace(incoming.limit_price);
        PriceLevel& level = level_it->second;

        try {
            level.orders.push_back(RestingOrder{
                .id = incoming.id,
                .side = incoming.side,
                .limit_price = incoming.limit_price,
                .original_quantity = incoming.quantity,
                .remaining_quantity = resting_quantity,
                .arrival_sequence = arrival_sequence,
            });
        } catch (...) {
            if (inserted_level) {
                levels.erase(level_it);
            }
            throw;
        }

        const auto resting_order = std::prev(level.orders.end());
        try {
            active_orders.emplace(
                incoming.id,
                OrderLocator{
                    .side = incoming.side,
                    .price = incoming.limit_price,
                    .order = resting_order,
                });
        } catch (...) {
            level.orders.pop_back();
            if (inserted_level) {
                levels.erase(level_it);
            }
            throw;
        }

        level.total_quantity.units += resting_quantity.units;
    }

    template <typename Levels, typename PriceCrosses>
    [[nodiscard]] static MatchPlan preview_match(
        const Levels& levels,
        Price incoming_price,
        Quantity incoming_quantity,
        PriceCrosses price_crosses) {
        Quantity remaining_quantity = incoming_quantity;
        std::size_t trade_count = 0;

        for (auto level_it = levels.begin();
             level_it != levels.end() && remaining_quantity.units > 0 &&
             price_crosses(level_it->first, incoming_price);
             ++level_it) {
            for (const RestingOrder& maker : level_it->second.orders) {
                if (trade_count == std::numeric_limits<std::size_t>::max()) {
                    return MatchPlan{
                        .remaining_quantity = remaining_quantity,
                        .trade_count = trade_count,
                    };
                }

                const std::uint64_t executed_quantity = std::min(
                    remaining_quantity.units,
                    maker.remaining_quantity.units);
                remaining_quantity.units -= executed_quantity;
                ++trade_count;

                if (remaining_quantity.units == 0) {
                    break;
                }
            }
        }

        return MatchPlan{
            .remaining_quantity = remaining_quantity,
            .trade_count = trade_count,
        };
    }

    [[nodiscard]] bool can_issue_trade_ids(std::size_t trade_count) const noexcept {
        std::uint64_t prospective_trade_id = next_trade_id;
        for (std::size_t index = 0; index < trade_count; ++index) {
            if (prospective_trade_id == 0) {
                return false;
            }
            ++prospective_trade_id;
        }
        return true;
    }

    template <typename Levels, typename PriceCrosses>
    void match(
        Levels& levels,
        const NewLimitOrder& incoming,
        Quantity& remaining_quantity,
        std::vector<Trade>& trades,
        PriceCrosses price_crosses) {
        while (!levels.empty() && remaining_quantity.units > 0 &&
               price_crosses(levels.begin()->first, incoming.limit_price)) {
            const auto level_it = levels.begin();
            PriceLevel& level = level_it->second;
            const auto maker_it = level.orders.begin();
            RestingOrder& maker = *maker_it;
            const std::uint64_t executed_quantity = std::min(
                remaining_quantity.units,
                maker.remaining_quantity.units);

            if (executed_quantity == 0 || level.total_quantity.units < executed_quantity ||
                next_trade_id == 0) {
                assert(false && "matching state or trade-ID counter is corrupt");
                std::terminate();
            }

            trades.push_back(Trade{
                .id = TradeId{next_trade_id},
                .maker_order_id = maker.id,
                .taker_order_id = incoming.id,
                .execution_price = maker.limit_price,
                .execution_quantity = Quantity{executed_quantity},
            });
            ++next_trade_id;

            remaining_quantity.units -= executed_quantity;
            maker.remaining_quantity.units -= executed_quantity;
            level.total_quantity.units -= executed_quantity;

            if (maker.remaining_quantity.units == 0) {
                const OrderId maker_id = maker.id;
                if (active_orders.erase(maker_id) != 1) {
                    assert(false && "fully filled maker is missing from the active-order index");
                    std::terminate();
                }
                level.orders.erase(maker_it);
                if (level.orders.empty()) {
                    levels.erase(level_it);
                }
            }
        }
    }

    template <typename Levels>
    [[nodiscard]] Quantity cancel_resting_order(
        Levels& levels,
        const OrderLocator& locator,
        OrderId id) {
        const auto level_it = levels.find(locator.price);
        if (level_it == levels.end() || locator.order->id != id ||
            locator.order->remaining_quantity.units == 0 ||
            level_it->second.total_quantity.units < locator.order->remaining_quantity.units) {
            assert(false && "active-order locator or aggregate quantity is corrupt");
            std::terminate();
        }

        PriceLevel& level = level_it->second;
        const Quantity cancelled_quantity = locator.order->remaining_quantity;
        level.total_quantity.units -= cancelled_quantity.units;
        level.orders.erase(locator.order);
        if (level.orders.empty()) {
            levels.erase(level_it);
        }
        return cancelled_quantity;
    }

    template <typename Levels>
    [[nodiscard]] static std::vector<PriceLevelView> depth_from(
        const Levels& levels,
        std::size_t max_levels) {
        std::vector<PriceLevelView> result;
        result.reserve(std::min(max_levels, levels.size()));

        for (const auto& [price, level] : levels) {
            if (result.size() == max_levels) {
                break;
            }
            result.push_back(PriceLevelView{
                .price = price,
                .total_quantity = level.total_quantity,
                .order_count = level.orders.size(),
            });
        }
        return result;
    }

    [[nodiscard]] bool invariants_hold() const {
        if (!bids.empty() && !asks.empty() && bids.begin()->first >= asks.begin()->first) {
            return false;
        }

        std::size_t counted_orders = 0;
        std::unordered_set<OrderId, OrderIdHash> orders_in_book;

        const auto check_side = [this, &counted_orders, &orders_in_book](const auto& levels, Side side) {
            std::optional<Price> previous_price;

            for (const auto& [price, level] : levels) {
                if (level.orders.empty()) {
                    return false;
                }
                if (previous_price.has_value()) {
                    if (side == Side::Buy && previous_price.value() <= price) {
                        return false;
                    }
                    if (side == Side::Sell && previous_price.value() >= price) {
                        return false;
                    }
                }
                previous_price = price;

                Quantity aggregate{};
                std::uint64_t previous_sequence{};
                for (auto order_it = level.orders.begin(); order_it != level.orders.end(); ++order_it) {
                    const RestingOrder& order = *order_it;
                    if (order.side != side || order.limit_price != price ||
                        order.remaining_quantity.units == 0 || order.original_quantity.units == 0 ||
                        order.remaining_quantity.units > order.original_quantity.units ||
                        order.arrival_sequence == 0 ||
                        (previous_sequence != 0 && order.arrival_sequence <= previous_sequence) ||
                        would_overflow(aggregate, order.remaining_quantity)) {
                        return false;
                    }
                    previous_sequence = order.arrival_sequence;
                    aggregate.units += order.remaining_quantity.units;

                    const auto locator = active_orders.find(order.id);
                    if (locator == active_orders.end() || locator->second.side != side ||
                        locator->second.price != price || locator->second.order != order_it ||
                        !seen_order_ids.contains(order.id) || !orders_in_book.insert(order.id).second) {
                        return false;
                    }
                    ++counted_orders;
                }
                if (aggregate != level.total_quantity) {
                    return false;
                }
            }
            return true;
        };

        return check_side(bids, Side::Buy) && check_side(asks, Side::Sell) &&
               counted_orders == active_orders.size();
    }

    template <typename Levels>
    [[nodiscard]] static std::vector<OrderId> order_ids_at(const Levels& levels, Price price) {
        const auto level_it = levels.find(price);
        if (level_it == levels.end()) {
            return {};
        }

        std::vector<OrderId> order_ids;
        order_ids.reserve(level_it->second.orders.size());
        for (const RestingOrder& order : level_it->second.orders) {
            order_ids.push_back(order.id);
        }
        return order_ids;
    }
};

OrderBook::OrderBook() : impl_(std::make_unique<Impl>()) {}

OrderBook::~OrderBook() = default;

SubmitResult OrderBook::submit(NewLimitOrder order) {
    if (order.id.value == 0) {
        return rejected(RejectionReason::InvalidOrderId);
    }
    if (!is_valid_side(order.side)) {
        return rejected(RejectionReason::InvalidSide);
    }
    if (order.limit_price.ticks == 0) {
        return rejected(RejectionReason::InvalidPrice);
    }
    if (order.quantity.units == 0) {
        return rejected(RejectionReason::InvalidQuantity);
    }
    if (impl_->seen_order_ids.contains(order.id)) {
        return rejected(RejectionReason::DuplicateOrderId);
    }

    const Impl::MatchPlan match_plan = order.side == Side::Buy
                                           ? Impl::preview_match(
                                                 impl_->asks,
                                                 order.limit_price,
                                                 order.quantity,
                                                 [](Price ask_price, Price incoming_price) {
                                                     return ask_price <= incoming_price;
                                                 })
                                           : Impl::preview_match(
                                                 impl_->bids,
                                                 order.limit_price,
                                                 order.quantity,
                                                 [](Price bid_price, Price incoming_price) {
                                                     return bid_price >= incoming_price;
                                                 });

    if (!impl_->can_issue_trade_ids(match_plan.trade_count)) {
        return rejected(RejectionReason::TradeIdExhausted);
    }
    if (match_plan.remaining_quantity.units > 0 && impl_->next_arrival_sequence == 0) {
        return rejected(RejectionReason::SequenceExhausted);
    }

    if (match_plan.remaining_quantity.units > 0) {
        if (order.side == Side::Buy) {
            const auto existing_level = impl_->bids.find(order.limit_price);
            if (existing_level != impl_->bids.end() &&
                would_overflow(existing_level->second.total_quantity, match_plan.remaining_quantity)) {
                return rejected(RejectionReason::QuantityOverflow);
            }
        } else {
            const auto existing_level = impl_->asks.find(order.limit_price);
            if (existing_level != impl_->asks.end() &&
                would_overflow(existing_level->second.total_quantity, match_plan.remaining_quantity)) {
                return rejected(RejectionReason::QuantityOverflow);
            }
        }
    }

    std::vector<Trade> trades;
    trades.reserve(match_plan.trade_count);

    const auto [seen_it, inserted_seen_id] = impl_->seen_order_ids.insert(order.id);
    assert(inserted_seen_id);
    static_cast<void>(seen_it);
    static_cast<void>(inserted_seen_id);

    {
        Quantity remaining_quantity = order.quantity;
        if (order.side == Side::Buy) {
            impl_->match(
                impl_->asks,
                order,
                remaining_quantity,
                trades,
                [](Price ask_price, Price incoming_price) { return ask_price <= incoming_price; });
        } else {
            impl_->match(
                impl_->bids,
                order,
                remaining_quantity,
                trades,
                [](Price bid_price, Price incoming_price) { return bid_price >= incoming_price; });
        }

        assert(remaining_quantity == match_plan.remaining_quantity);
        assert(trades.size() == match_plan.trade_count);

        if (remaining_quantity.units > 0) {
            if (order.side == Side::Buy) {
                impl_->insert_resting_order(
                    impl_->bids,
                    order,
                    remaining_quantity,
                    impl_->next_arrival_sequence);
            } else {
                impl_->insert_resting_order(
                    impl_->asks,
                    order,
                    remaining_quantity,
                    impl_->next_arrival_sequence);
            }
            ++impl_->next_arrival_sequence;
        }
    }
    assert(impl_->invariants_hold());

    return SubmitResult{
        .status = SubmissionStatus::Accepted,
        .rejection_reason = std::nullopt,
        .executed_quantity = Quantity{order.quantity.units - match_plan.remaining_quantity.units},
        .resting_quantity = match_plan.remaining_quantity,
        .trades = std::move(trades),
    };
}

CancelResult OrderBook::cancel(OrderId id) {
    const auto active_order = impl_->active_orders.find(id);
    if (active_order == impl_->active_orders.end()) {
        return CancelResult{
            .status = CancelStatus::NotFound,
            .cancelled_quantity = Quantity{0},
        };
    }

    const Impl::OrderLocator locator = active_order->second;
    const Quantity cancelled_quantity = locator.side == Side::Buy
                                            ? impl_->cancel_resting_order(impl_->bids, locator, id)
                                            : impl_->cancel_resting_order(impl_->asks, locator, id);
    impl_->active_orders.erase(active_order);

    assert(impl_->invariants_hold());
    return CancelResult{
        .status = CancelStatus::Cancelled,
        .cancelled_quantity = cancelled_quantity,
    };
}

std::optional<Quote> OrderBook::best_bid() const {
    if (impl_->bids.empty()) {
        return std::nullopt;
    }
    const auto& [price, level] = *impl_->bids.begin();
    return Quote{.price = price, .quantity = level.total_quantity};
}

std::optional<Quote> OrderBook::best_ask() const {
    if (impl_->asks.empty()) {
        return std::nullopt;
    }
    const auto& [price, level] = *impl_->asks.begin();
    return Quote{.price = price, .quantity = level.total_quantity};
}

std::optional<OrderView> OrderBook::find(OrderId id) const {
    const auto locator = impl_->active_orders.find(id);
    if (locator == impl_->active_orders.end()) {
        return std::nullopt;
    }

    const Impl::RestingOrder& order = *locator->second.order;
    return OrderView{
        .id = order.id,
        .side = order.side,
        .limit_price = order.limit_price,
        .original_quantity = order.original_quantity,
        .remaining_quantity = order.remaining_quantity,
    };
}

std::vector<PriceLevelView> OrderBook::depth(Side side, std::size_t max_levels) const {
    if (max_levels == 0 || !is_valid_side(side)) {
        return {};
    }
    if (side == Side::Buy) {
        return Impl::depth_from(impl_->bids, max_levels);
    }
    return Impl::depth_from(impl_->asks, max_levels);
}

std::size_t OrderBook::active_order_count() const noexcept {
    return impl_->active_orders.size();
}

bool OrderBook::invariants_hold_for_testing() const {
    return impl_->invariants_hold();
}

std::vector<OrderId> OrderBook::order_ids_at_for_testing(Side side, Price price) const {
    if (side == Side::Buy) {
        return Impl::order_ids_at(impl_->bids, price);
    }
    if (side == Side::Sell) {
        return Impl::order_ids_at(impl_->asks, price);
    }
    return {};
}

void OrderBook::set_next_trade_id_for_testing(TradeId id) {
    impl_->next_trade_id = id.value;
}

} // namespace matching
