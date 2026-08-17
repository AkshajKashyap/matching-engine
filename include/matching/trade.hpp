#pragma once

#include "matching/types.hpp"

namespace matching {

struct Trade {
    TradeId id;
    OrderId maker_order_id;
    OrderId taker_order_id;
    Price execution_price;
    Quantity execution_quantity;
};

} // namespace matching
