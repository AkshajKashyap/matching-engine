#pragma once

#include "matching/types.hpp"

namespace matching {

// The input to submit(). A residual quantity will eventually rest as GTC.
struct NewLimitOrder {
    OrderId id;
    Side side;
    Price limit_price;
    Quantity quantity;
};

// A value snapshot of an active order. It deliberately exposes no book internals.
struct OrderView {
    OrderId id;
    Side side;
    Price limit_price;
    Quantity original_quantity;
    Quantity remaining_quantity;
};

} // namespace matching
