#pragma once

#include <vector>

#include "matching/order_book.hpp"

namespace matching::testing {

struct OrderBookTestAccess {
    [[nodiscard]] static bool invariants_hold(const OrderBook& book) {
        return book.invariants_hold_for_testing();
    }

    [[nodiscard]] static std::vector<OrderId> order_ids_at(
        const OrderBook& book,
        Side side,
        Price price) {
        return book.order_ids_at_for_testing(side, price);
    }

    static void set_next_trade_id(OrderBook& book, TradeId id) {
        book.set_next_trade_id_for_testing(id);
    }
};

} // namespace matching::testing
