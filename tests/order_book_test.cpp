#include <cstdint>
#include <limits>
#include <unordered_set>
#include <vector>

#include <gtest/gtest.h>

#include "matching/order.hpp"
#include "matching/order_book.hpp"
#include "matching/trade.hpp"
#include "matching/types.hpp"
#include "order_book_test_access.hpp"

namespace matching {
namespace {

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

void expect_invariants(const OrderBook& book) {
    EXPECT_TRUE(testing::OrderBookTestAccess::invariants_hold(book));
}

TEST(StrongUnsignedTypesTest, PreserveValueEqualityAndOrdering) {
    const Price lower_price{99};
    const Price higher_price{100};
    const Quantity smaller_quantity{7};
    const Quantity equal_quantity{7};

    EXPECT_LT(lower_price, higher_price);
    EXPECT_EQ(smaller_quantity, equal_quantity);
    EXPECT_NE(OrderId{1}, OrderId{2});
    EXPECT_EQ(TradeId{42}.value, std::uint64_t{42});
}

TEST(StrongUnsignedTypesTest, OrderIdHashSupportsUnorderedContainers) {
    std::unordered_set<OrderId, OrderIdHash> ids;
    ids.insert(OrderId{11});

    EXPECT_TRUE(ids.contains(OrderId{11}));
    EXPECT_FALSE(ids.contains(OrderId{12}));
}

TEST(PublicDomainObjectsTest, ConstructOrderAndTradeValues) {
    const NewLimitOrder order{
        .id = OrderId{101},
        .side = Side::Buy,
        .limit_price = Price{2500},
        .quantity = Quantity{4},
    };
    const OrderView view{
        .id = order.id,
        .side = order.side,
        .limit_price = order.limit_price,
        .original_quantity = order.quantity,
        .remaining_quantity = Quantity{3},
    };
    const Trade trade{
        .id = TradeId{1},
        .maker_order_id = OrderId{100},
        .taker_order_id = order.id,
        .execution_price = Price{2499},
        .execution_quantity = Quantity{1},
    };

    EXPECT_EQ(view.remaining_quantity, Quantity{3});
    EXPECT_EQ(trade.taker_order_id, order.id);
    EXPECT_EQ(trade.execution_price, Price{2499});
}

TEST(OrderBookTest, FreshBookHasNoVisibleState) {
    const OrderBook book;

    EXPECT_FALSE(book.best_bid().has_value());
    EXPECT_FALSE(book.best_ask().has_value());
    EXPECT_FALSE(book.find(OrderId{1}).has_value());
    EXPECT_TRUE(book.depth(Side::Buy, 10).empty());
    EXPECT_TRUE(book.depth(Side::Sell, 10).empty());
    EXPECT_EQ(book.active_order_count(), std::size_t{0});
    expect_invariants(book);
}

TEST(PublicResultTypesTest, DefaultSubmitResultIsNotAccepted) {
    const SubmitResult result;

    EXPECT_FALSE(result.accepted());
    EXPECT_FALSE(result.rejection_reason.has_value());
    EXPECT_TRUE(result.trades.empty());
}

TEST(OrderBookTest, StoresAndQueriesASinglePassiveBid) {
    OrderBook book;

    const SubmitResult result = book.submit(order(1, Side::Buy, 100, 7));

    ASSERT_TRUE(result.accepted());
    EXPECT_FALSE(result.rejection_reason.has_value());
    EXPECT_EQ(result.executed_quantity, Quantity{0});
    EXPECT_EQ(result.resting_quantity, Quantity{7});
    EXPECT_TRUE(result.trades.empty());

    const auto best_bid = book.best_bid();
    ASSERT_TRUE(best_bid.has_value());
    EXPECT_EQ(best_bid->price, Price{100});
    EXPECT_EQ(best_bid->quantity, Quantity{7});
    EXPECT_FALSE(book.best_ask().has_value());
    EXPECT_EQ(book.active_order_count(), std::size_t{1});

    const auto found = book.find(OrderId{1});
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->id, OrderId{1});
    EXPECT_EQ(found->side, Side::Buy);
    EXPECT_EQ(found->limit_price, Price{100});
    EXPECT_EQ(found->original_quantity, Quantity{7});
    EXPECT_EQ(found->remaining_quantity, Quantity{7});
    expect_invariants(book);
}

TEST(OrderBookTest, StoresAndQueriesASinglePassiveAsk) {
    OrderBook book;

    ASSERT_TRUE(book.submit(order(1, Side::Sell, 101, 8)).accepted());

    EXPECT_FALSE(book.best_bid().has_value());
    const auto best_ask = book.best_ask();
    ASSERT_TRUE(best_ask.has_value());
    EXPECT_EQ(best_ask->price, Price{101});
    EXPECT_EQ(best_ask->quantity, Quantity{8});

    const auto ask_depth = book.depth(Side::Sell, 1);
    ASSERT_EQ(ask_depth.size(), std::size_t{1});
    EXPECT_EQ(ask_depth[0].price, Price{101});
    EXPECT_EQ(ask_depth[0].total_quantity, Quantity{8});
    EXPECT_EQ(ask_depth[0].order_count, std::size_t{1});
    expect_invariants(book);
}

TEST(OrderBookTest, ReturnsBidsAndAsksBestToWorst) {
    OrderBook book;

    ASSERT_TRUE(book.submit(order(1, Side::Buy, 99, 1)).accepted());
    ASSERT_TRUE(book.submit(order(2, Side::Buy, 101, 2)).accepted());
    ASSERT_TRUE(book.submit(order(3, Side::Buy, 100, 3)).accepted());
    ASSERT_TRUE(book.submit(order(4, Side::Sell, 105, 4)).accepted());
    ASSERT_TRUE(book.submit(order(5, Side::Sell, 103, 5)).accepted());
    ASSERT_TRUE(book.submit(order(6, Side::Sell, 104, 6)).accepted());

    const auto best_bid = book.best_bid();
    const auto best_ask = book.best_ask();
    ASSERT_TRUE(best_bid.has_value());
    ASSERT_TRUE(best_ask.has_value());
    EXPECT_EQ(best_bid->price, Price{101});
    EXPECT_EQ(best_ask->price, Price{103});

    const auto bids = book.depth(Side::Buy, 10);
    ASSERT_EQ(bids.size(), std::size_t{3});
    EXPECT_EQ(bids[0].price, Price{101});
    EXPECT_EQ(bids[1].price, Price{100});
    EXPECT_EQ(bids[2].price, Price{99});

    const auto asks = book.depth(Side::Sell, 10);
    ASSERT_EQ(asks.size(), std::size_t{3});
    EXPECT_EQ(asks[0].price, Price{103});
    EXPECT_EQ(asks[1].price, Price{104});
    EXPECT_EQ(asks[2].price, Price{105});
    expect_invariants(book);
}

TEST(OrderBookTest, MaintainsFifoAndAggregateQuantityAtOnePriceLevel) {
    OrderBook book;

    ASSERT_TRUE(book.submit(order(10, Side::Buy, 100, 3)).accepted());
    ASSERT_TRUE(book.submit(order(11, Side::Buy, 100, 5)).accepted());
    ASSERT_TRUE(book.submit(order(12, Side::Buy, 100, 7)).accepted());

    const auto bid_depth = book.depth(Side::Buy, 1);
    ASSERT_EQ(bid_depth.size(), std::size_t{1});
    EXPECT_EQ(bid_depth[0].total_quantity, Quantity{15});
    EXPECT_EQ(bid_depth[0].order_count, std::size_t{3});

    const std::vector<OrderId> order_ids =
        testing::OrderBookTestAccess::order_ids_at(book, Side::Buy, Price{100});
    ASSERT_EQ(order_ids.size(), std::size_t{3});
    EXPECT_EQ(order_ids[0], OrderId{10});
    EXPECT_EQ(order_ids[1], OrderId{11});
    EXPECT_EQ(order_ids[2], OrderId{12});
    expect_invariants(book);
}

TEST(OrderBookTest, TruncatesDepthAndHandlesZeroMaxLevels) {
    OrderBook book;

    ASSERT_TRUE(book.submit(order(1, Side::Sell, 102, 1)).accepted());
    ASSERT_TRUE(book.submit(order(2, Side::Sell, 101, 1)).accepted());
    ASSERT_TRUE(book.submit(order(3, Side::Sell, 103, 1)).accepted());

    EXPECT_TRUE(book.depth(Side::Sell, 0).empty());
    const auto top_two = book.depth(Side::Sell, 2);
    ASSERT_EQ(top_two.size(), std::size_t{2});
    EXPECT_EQ(top_two[0].price, Price{101});
    EXPECT_EQ(top_two[1].price, Price{102});
    expect_invariants(book);
}

TEST(OrderBookTest, RejectsZeroOrderIdWithoutMutatingTheBook) {
    OrderBook book;

    const SubmitResult result = book.submit(order(0, Side::Buy, 100, 1));

    EXPECT_FALSE(result.accepted());
    EXPECT_EQ(result.rejection_reason, RejectionReason::InvalidOrderId);
    EXPECT_EQ(book.active_order_count(), std::size_t{0});
    EXPECT_FALSE(book.find(OrderId{0}).has_value());
    expect_invariants(book);
}

TEST(OrderBookTest, RejectsZeroPriceAndZeroQuantityWithoutMutatingTheBook) {
    OrderBook book;

    const SubmitResult zero_price = book.submit(order(1, Side::Buy, 0, 1));
    const SubmitResult zero_quantity = book.submit(order(2, Side::Sell, 101, 0));

    EXPECT_EQ(zero_price.rejection_reason, RejectionReason::InvalidPrice);
    EXPECT_EQ(zero_quantity.rejection_reason, RejectionReason::InvalidQuantity);
    EXPECT_EQ(book.active_order_count(), std::size_t{0});
    expect_invariants(book);
}

TEST(OrderBookTest, RejectsDuplicateActiveAndPreviouslyAcceptedOrderIds) {
    OrderBook book;

    ASSERT_TRUE(book.submit(order(7, Side::Buy, 100, 2)).accepted());
    const SubmitResult duplicate_active = book.submit(order(7, Side::Sell, 102, 3));
    const SubmitResult previously_accepted = book.submit(order(7, Side::Buy, 99, 4));

    EXPECT_EQ(duplicate_active.rejection_reason, RejectionReason::DuplicateOrderId);
    EXPECT_EQ(previously_accepted.rejection_reason, RejectionReason::DuplicateOrderId);
    EXPECT_EQ(book.active_order_count(), std::size_t{1});
    const auto found = book.find(OrderId{7});
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->limit_price, Price{100});
    expect_invariants(book);
}

TEST(OrderBookTest, RejectsAggregateQuantityOverflowAtomically) {
    OrderBook book;
    const std::uint64_t maximum_quantity = std::numeric_limits<std::uint64_t>::max();

    ASSERT_TRUE(book.submit(order(1, Side::Buy, 100, maximum_quantity)).accepted());
    const SubmitResult overflow = book.submit(order(2, Side::Buy, 100, 1));

    EXPECT_FALSE(overflow.accepted());
    EXPECT_EQ(overflow.rejection_reason, RejectionReason::QuantityOverflow);
    const auto best_bid = book.best_bid();
    ASSERT_TRUE(best_bid.has_value());
    EXPECT_EQ(best_bid->quantity, Quantity{maximum_quantity});
    EXPECT_EQ(book.active_order_count(), std::size_t{1});

    EXPECT_TRUE(book.submit(order(2, Side::Buy, 99, 1)).accepted());
    expect_invariants(book);
}

TEST(OrderBookTest, PreservesStateWhenARejectionFollowsAcceptedOrders) {
    OrderBook book;

    ASSERT_TRUE(book.submit(order(1, Side::Buy, 100, 4)).accepted());
    ASSERT_TRUE(book.submit(order(2, Side::Sell, 102, 5)).accepted());
    const auto bids_before = book.depth(Side::Buy, 10);
    const auto asks_before = book.depth(Side::Sell, 10);

    const SubmitResult invalid = book.submit(order(0, Side::Buy, 101, 1));
    const SubmitResult duplicate = book.submit(order(1, Side::Sell, 103, 1));

    EXPECT_EQ(invalid.rejection_reason, RejectionReason::InvalidOrderId);
    EXPECT_EQ(duplicate.rejection_reason, RejectionReason::DuplicateOrderId);
    const auto bids_after = book.depth(Side::Buy, 10);
    const auto asks_after = book.depth(Side::Sell, 10);
    ASSERT_EQ(bids_after.size(), bids_before.size());
    ASSERT_EQ(asks_after.size(), asks_before.size());
    EXPECT_EQ(bids_after[0].price, bids_before[0].price);
    EXPECT_EQ(bids_after[0].total_quantity, bids_before[0].total_quantity);
    EXPECT_EQ(asks_after[0].price, asks_before[0].price);
    EXPECT_EQ(asks_after[0].total_quantity, asks_before[0].total_quantity);
    expect_invariants(book);
}

TEST(OrderBookTest, InvariantsHoldAfterRepresentativePassiveSequence) {
    OrderBook book;

    ASSERT_TRUE(book.submit(order(1, Side::Buy, 100, 2)).accepted());
    expect_invariants(book);
    ASSERT_TRUE(book.submit(order(2, Side::Buy, 99, 3)).accepted());
    expect_invariants(book);
    ASSERT_TRUE(book.submit(order(3, Side::Sell, 104, 4)).accepted());
    expect_invariants(book);
    ASSERT_TRUE(book.submit(order(4, Side::Buy, 100, 5)).accepted());
    expect_invariants(book);
    ASSERT_TRUE(book.submit(order(5, Side::Sell, 101, 6)).accepted());
    expect_invariants(book);
}

TEST(OrderBookCancellationTest, CancelUnknownIdReturnsNotFoundWithoutMutatingTheBook) {
    OrderBook book;
    ASSERT_TRUE(book.submit(order(1, Side::Buy, 100, 4)).accepted());
    const auto bids_before = book.depth(Side::Buy, 10);

    const CancelResult result = book.cancel(OrderId{999});

    EXPECT_EQ(result.status, CancelStatus::NotFound);
    EXPECT_EQ(result.cancelled_quantity, Quantity{0});
    EXPECT_EQ(book.active_order_count(), std::size_t{1});
    const auto bids_after = book.depth(Side::Buy, 10);
    ASSERT_EQ(bids_after.size(), std::size_t{1});
    EXPECT_EQ(bids_after[0].price, bids_before[0].price);
    EXPECT_EQ(bids_after[0].total_quantity, bids_before[0].total_quantity);
    expect_invariants(book);
}

TEST(OrderBookCancellationTest, CancelSoleBidRemovesLevelAndPreventsIdReuse) {
    OrderBook book;
    ASSERT_TRUE(book.submit(order(42, Side::Buy, 100, 7)).accepted());

    const CancelResult cancelled = book.cancel(OrderId{42});

    EXPECT_EQ(cancelled.status, CancelStatus::Cancelled);
    EXPECT_EQ(cancelled.cancelled_quantity, Quantity{7});
    EXPECT_FALSE(book.best_bid().has_value());
    EXPECT_TRUE(book.depth(Side::Buy, 10).empty());
    EXPECT_FALSE(book.find(OrderId{42}).has_value());
    EXPECT_EQ(book.active_order_count(), std::size_t{0});

    const SubmitResult reused = book.submit(order(42, Side::Buy, 100, 1));
    EXPECT_EQ(reused.rejection_reason, RejectionReason::DuplicateOrderId);
    expect_invariants(book);
}

TEST(OrderBookCancellationTest, CancelSoleAskRemovesLevel) {
    OrderBook book;
    ASSERT_TRUE(book.submit(order(1, Side::Sell, 101, 8)).accepted());

    const CancelResult cancelled = book.cancel(OrderId{1});

    EXPECT_EQ(cancelled.status, CancelStatus::Cancelled);
    EXPECT_EQ(cancelled.cancelled_quantity, Quantity{8});
    EXPECT_FALSE(book.best_ask().has_value());
    EXPECT_TRUE(book.depth(Side::Sell, 10).empty());
    EXPECT_FALSE(book.find(OrderId{1}).has_value());
    EXPECT_EQ(book.active_order_count(), std::size_t{0});
    expect_invariants(book);
}

TEST(OrderBookCancellationTest, CancelHeadPreservesFifoAndUpdatesAggregate) {
    OrderBook book;
    ASSERT_TRUE(book.submit(order(1, Side::Buy, 100, 2)).accepted());
    ASSERT_TRUE(book.submit(order(2, Side::Buy, 100, 3)).accepted());
    ASSERT_TRUE(book.submit(order(3, Side::Buy, 100, 5)).accepted());

    const CancelResult cancelled = book.cancel(OrderId{1});

    EXPECT_EQ(cancelled.status, CancelStatus::Cancelled);
    EXPECT_EQ(cancelled.cancelled_quantity, Quantity{2});
    const auto depth = book.depth(Side::Buy, 1);
    ASSERT_EQ(depth.size(), std::size_t{1});
    EXPECT_EQ(depth[0].total_quantity, Quantity{8});
    EXPECT_EQ(depth[0].order_count, std::size_t{2});
    const auto order_ids = testing::OrderBookTestAccess::order_ids_at(book, Side::Buy, Price{100});
    ASSERT_EQ(order_ids.size(), std::size_t{2});
    EXPECT_EQ(order_ids[0], OrderId{2});
    EXPECT_EQ(order_ids[1], OrderId{3});
    EXPECT_FALSE(book.find(OrderId{1}).has_value());
    expect_invariants(book);
}

TEST(OrderBookCancellationTest, CancelMiddlePreservesFifoAndOtherOrders) {
    OrderBook book;
    ASSERT_TRUE(book.submit(order(1, Side::Buy, 100, 2)).accepted());
    ASSERT_TRUE(book.submit(order(2, Side::Buy, 100, 3)).accepted());
    ASSERT_TRUE(book.submit(order(3, Side::Buy, 100, 5)).accepted());
    ASSERT_TRUE(book.submit(order(4, Side::Buy, 100, 7)).accepted());

    const CancelResult cancelled = book.cancel(OrderId{2});

    EXPECT_EQ(cancelled.status, CancelStatus::Cancelled);
    EXPECT_EQ(cancelled.cancelled_quantity, Quantity{3});
    const auto order_ids = testing::OrderBookTestAccess::order_ids_at(book, Side::Buy, Price{100});
    ASSERT_EQ(order_ids.size(), std::size_t{3});
    EXPECT_EQ(order_ids[0], OrderId{1});
    EXPECT_EQ(order_ids[1], OrderId{3});
    EXPECT_EQ(order_ids[2], OrderId{4});
    const auto depth = book.depth(Side::Buy, 1);
    ASSERT_EQ(depth.size(), std::size_t{1});
    EXPECT_EQ(depth[0].total_quantity, Quantity{14});
    EXPECT_EQ(depth[0].order_count, std::size_t{3});
    ASSERT_TRUE(book.find(OrderId{3}).has_value());
    ASSERT_TRUE(book.find(OrderId{4}).has_value());
    expect_invariants(book);
}

TEST(OrderBookCancellationTest, CancelTailPreservesFifo) {
    OrderBook book;
    ASSERT_TRUE(book.submit(order(1, Side::Sell, 105, 2)).accepted());
    ASSERT_TRUE(book.submit(order(2, Side::Sell, 105, 3)).accepted());
    ASSERT_TRUE(book.submit(order(3, Side::Sell, 105, 5)).accepted());

    const CancelResult cancelled = book.cancel(OrderId{3});

    EXPECT_EQ(cancelled.status, CancelStatus::Cancelled);
    EXPECT_EQ(cancelled.cancelled_quantity, Quantity{5});
    const auto order_ids = testing::OrderBookTestAccess::order_ids_at(book, Side::Sell, Price{105});
    ASSERT_EQ(order_ids.size(), std::size_t{2});
    EXPECT_EQ(order_ids[0], OrderId{1});
    EXPECT_EQ(order_ids[1], OrderId{2});
    expect_invariants(book);
}

TEST(OrderBookCancellationTest, CancelBestBidAndBestAskExposeNextLevels) {
    OrderBook book;
    ASSERT_TRUE(book.submit(order(1, Side::Buy, 101, 2)).accepted());
    ASSERT_TRUE(book.submit(order(2, Side::Buy, 100, 3)).accepted());
    ASSERT_TRUE(book.submit(order(3, Side::Sell, 103, 4)).accepted());
    ASSERT_TRUE(book.submit(order(4, Side::Sell, 104, 5)).accepted());

    EXPECT_EQ(book.cancel(OrderId{1}).status, CancelStatus::Cancelled);
    EXPECT_EQ(book.cancel(OrderId{3}).status, CancelStatus::Cancelled);

    const auto best_bid = book.best_bid();
    const auto best_ask = book.best_ask();
    ASSERT_TRUE(best_bid.has_value());
    ASSERT_TRUE(best_ask.has_value());
    EXPECT_EQ(best_bid->price, Price{100});
    EXPECT_EQ(best_bid->quantity, Quantity{3});
    EXPECT_EQ(best_ask->price, Price{104});
    EXPECT_EQ(best_ask->quantity, Quantity{5});
    expect_invariants(book);
}

TEST(OrderBookCancellationTest, CancelAtNonBestLevelDoesNotChangeBestQuote) {
    OrderBook book;
    ASSERT_TRUE(book.submit(order(1, Side::Buy, 101, 2)).accepted());
    ASSERT_TRUE(book.submit(order(2, Side::Buy, 100, 3)).accepted());
    ASSERT_TRUE(book.submit(order(3, Side::Sell, 103, 4)).accepted());
    ASSERT_TRUE(book.submit(order(4, Side::Sell, 104, 5)).accepted());

    EXPECT_EQ(book.cancel(OrderId{2}).status, CancelStatus::Cancelled);
    EXPECT_EQ(book.cancel(OrderId{4}).status, CancelStatus::Cancelled);

    const auto best_bid = book.best_bid();
    const auto best_ask = book.best_ask();
    ASSERT_TRUE(best_bid.has_value());
    ASSERT_TRUE(best_ask.has_value());
    EXPECT_EQ(best_bid->price, Price{101});
    EXPECT_EQ(best_ask->price, Price{103});
    expect_invariants(book);
}

TEST(OrderBookCancellationTest, RepeatedCancellationReturnsNotFound) {
    OrderBook book;
    ASSERT_TRUE(book.submit(order(1, Side::Buy, 100, 2)).accepted());

    const CancelResult first = book.cancel(OrderId{1});
    const CancelResult second = book.cancel(OrderId{1});

    EXPECT_EQ(first.status, CancelStatus::Cancelled);
    EXPECT_EQ(first.cancelled_quantity, Quantity{2});
    EXPECT_EQ(second.status, CancelStatus::NotFound);
    EXPECT_EQ(second.cancelled_quantity, Quantity{0});
    EXPECT_EQ(book.active_order_count(), std::size_t{0});
    expect_invariants(book);
}

TEST(OrderBookCancellationTest, InvariantsHoldAfterMultipleCancellations) {
    OrderBook book;
    ASSERT_TRUE(book.submit(order(1, Side::Buy, 101, 2)).accepted());
    ASSERT_TRUE(book.submit(order(2, Side::Buy, 100, 3)).accepted());
    ASSERT_TRUE(book.submit(order(3, Side::Buy, 100, 5)).accepted());
    ASSERT_TRUE(book.submit(order(4, Side::Sell, 104, 7)).accepted());
    ASSERT_TRUE(book.submit(order(5, Side::Sell, 105, 11)).accepted());

    ASSERT_EQ(book.cancel(OrderId{3}).status, CancelStatus::Cancelled);
    expect_invariants(book);
    ASSERT_EQ(book.cancel(OrderId{1}).status, CancelStatus::Cancelled);
    expect_invariants(book);
    ASSERT_EQ(book.cancel(OrderId{4}).status, CancelStatus::Cancelled);
    expect_invariants(book);
    ASSERT_EQ(book.cancel(OrderId{2}).status, CancelStatus::Cancelled);
    expect_invariants(book);
    ASSERT_EQ(book.cancel(OrderId{5}).status, CancelStatus::Cancelled);
    expect_invariants(book);
    EXPECT_FALSE(book.best_bid().has_value());
    EXPECT_FALSE(book.best_ask().has_value());
}

void expect_trade(
    const Trade& trade,
    std::uint64_t trade_id,
    std::uint64_t maker_order_id,
    std::uint64_t taker_order_id,
    std::uint64_t price,
    std::uint64_t quantity) {
    EXPECT_EQ(trade.id, TradeId{trade_id});
    EXPECT_EQ(trade.maker_order_id, OrderId{maker_order_id});
    EXPECT_EQ(trade.taker_order_id, OrderId{taker_order_id});
    EXPECT_EQ(trade.execution_price, Price{price});
    EXPECT_EQ(trade.execution_quantity, Quantity{quantity});
    EXPECT_GT(trade.execution_quantity.units, std::uint64_t{0});
}

TEST(OrderBookMatchingTest, BuyCrossesOneAskAtMakerPrice) {
    OrderBook book;
    ASSERT_TRUE(book.submit(order(1, Side::Sell, 100, 5)).accepted());

    const SubmitResult result = book.submit(order(2, Side::Buy, 105, 5));

    ASSERT_TRUE(result.accepted());
    EXPECT_EQ(result.executed_quantity, Quantity{5});
    EXPECT_EQ(result.resting_quantity, Quantity{0});
    ASSERT_EQ(result.trades.size(), std::size_t{1});
    expect_trade(result.trades[0], 1, 1, 2, 100, 5);
    EXPECT_FALSE(book.best_bid().has_value());
    EXPECT_FALSE(book.best_ask().has_value());
    EXPECT_FALSE(book.find(OrderId{1}).has_value());
    EXPECT_FALSE(book.find(OrderId{2}).has_value());
    EXPECT_EQ(book.active_order_count(), std::size_t{0});
    expect_invariants(book);
}

TEST(OrderBookMatchingTest, SellCrossesOneBidAtMakerPrice) {
    OrderBook book;
    ASSERT_TRUE(book.submit(order(1, Side::Buy, 105, 5)).accepted());

    const SubmitResult result = book.submit(order(2, Side::Sell, 100, 5));

    ASSERT_TRUE(result.accepted());
    EXPECT_EQ(result.executed_quantity, Quantity{5});
    EXPECT_EQ(result.resting_quantity, Quantity{0});
    ASSERT_EQ(result.trades.size(), std::size_t{1});
    expect_trade(result.trades[0], 1, 1, 2, 105, 5);
    EXPECT_FALSE(book.find(OrderId{1}).has_value());
    EXPECT_FALSE(book.find(OrderId{2}).has_value());
    expect_invariants(book);
}

TEST(OrderBookMatchingTest, ExactPriceBuyAndSellCrossesExecute) {
    OrderBook buy_book;
    ASSERT_TRUE(buy_book.submit(order(1, Side::Sell, 100, 2)).accepted());
    const SubmitResult buy = buy_book.submit(order(2, Side::Buy, 100, 2));
    ASSERT_EQ(buy.trades.size(), std::size_t{1});
    expect_trade(buy.trades[0], 1, 1, 2, 100, 2);
    EXPECT_FALSE(buy_book.best_bid().has_value());
    EXPECT_FALSE(buy_book.best_ask().has_value());

    OrderBook sell_book;
    ASSERT_TRUE(sell_book.submit(order(1, Side::Buy, 100, 2)).accepted());
    const SubmitResult sell = sell_book.submit(order(2, Side::Sell, 100, 2));
    ASSERT_EQ(sell.trades.size(), std::size_t{1});
    expect_trade(sell.trades[0], 1, 1, 2, 100, 2);
    EXPECT_FALSE(sell_book.best_bid().has_value());
    EXPECT_FALSE(sell_book.best_ask().has_value());
    expect_invariants(buy_book);
    expect_invariants(sell_book);
}

TEST(OrderBookMatchingTest, PartialIncomingFillLeavesMakerAtFrontAndCancelable) {
    OrderBook book;
    ASSERT_TRUE(book.submit(order(1, Side::Sell, 100, 10)).accepted());
    ASSERT_TRUE(book.submit(order(2, Side::Sell, 100, 3)).accepted());

    const SubmitResult result = book.submit(order(3, Side::Buy, 100, 4));

    ASSERT_EQ(result.trades.size(), std::size_t{1});
    expect_trade(result.trades[0], 1, 1, 3, 100, 4);
    const auto maker = book.find(OrderId{1});
    ASSERT_TRUE(maker.has_value());
    EXPECT_EQ(maker->remaining_quantity, Quantity{6});
    const auto order_ids = testing::OrderBookTestAccess::order_ids_at(book, Side::Sell, Price{100});
    ASSERT_EQ(order_ids.size(), std::size_t{2});
    EXPECT_EQ(order_ids[0], OrderId{1});
    EXPECT_EQ(order_ids[1], OrderId{2});
    const auto ask_depth = book.depth(Side::Sell, 1);
    ASSERT_EQ(ask_depth.size(), std::size_t{1});
    EXPECT_EQ(ask_depth[0].total_quantity, Quantity{9});

    const CancelResult cancelled = book.cancel(OrderId{1});
    EXPECT_EQ(cancelled.status, CancelStatus::Cancelled);
    EXPECT_EQ(cancelled.cancelled_quantity, Quantity{6});
    expect_invariants(book);
}

TEST(OrderBookMatchingTest, IncomingResidualRestsWithItsOriginalQuantityRecorded) {
    OrderBook book;
    ASSERT_TRUE(book.submit(order(1, Side::Sell, 100, 4)).accepted());

    const SubmitResult result = book.submit(order(2, Side::Buy, 100, 10));

    ASSERT_EQ(result.trades.size(), std::size_t{1});
    expect_trade(result.trades[0], 1, 1, 2, 100, 4);
    EXPECT_EQ(result.executed_quantity, Quantity{4});
    EXPECT_EQ(result.resting_quantity, Quantity{6});
    const auto incoming = book.find(OrderId{2});
    ASSERT_TRUE(incoming.has_value());
    EXPECT_EQ(incoming->original_quantity, Quantity{10});
    EXPECT_EQ(incoming->remaining_quantity, Quantity{6});
    const auto best_bid = book.best_bid();
    ASSERT_TRUE(best_bid.has_value());
    EXPECT_EQ(best_bid->price, Price{100});
    EXPECT_EQ(best_bid->quantity, Quantity{6});
    EXPECT_FALSE(book.best_ask().has_value());
    expect_invariants(book);
}

TEST(OrderBookMatchingTest, ConsumesRestingOrdersInFifoOrderAtTheSamePrice) {
    OrderBook book;
    ASSERT_TRUE(book.submit(order(1, Side::Sell, 100, 2)).accepted());
    ASSERT_TRUE(book.submit(order(2, Side::Sell, 100, 3)).accepted());
    ASSERT_TRUE(book.submit(order(3, Side::Sell, 100, 4)).accepted());

    const SubmitResult result = book.submit(order(4, Side::Buy, 100, 4));

    ASSERT_EQ(result.trades.size(), std::size_t{2});
    expect_trade(result.trades[0], 1, 1, 4, 100, 2);
    expect_trade(result.trades[1], 2, 2, 4, 100, 2);
    EXPECT_FALSE(book.find(OrderId{1}).has_value());
    const auto second_maker = book.find(OrderId{2});
    ASSERT_TRUE(second_maker.has_value());
    EXPECT_EQ(second_maker->remaining_quantity, Quantity{1});
    const auto order_ids = testing::OrderBookTestAccess::order_ids_at(book, Side::Sell, Price{100});
    ASSERT_EQ(order_ids.size(), std::size_t{2});
    EXPECT_EQ(order_ids[0], OrderId{2});
    EXPECT_EQ(order_ids[1], OrderId{3});
    expect_invariants(book);
}

TEST(OrderBookMatchingTest, ExhaustingAtFifoBoundaryLeavesNextMakerUntouched) {
    OrderBook book;
    ASSERT_TRUE(book.submit(order(1, Side::Sell, 100, 2)).accepted());
    ASSERT_TRUE(book.submit(order(2, Side::Sell, 100, 3)).accepted());
    ASSERT_TRUE(book.submit(order(3, Side::Sell, 100, 4)).accepted());

    const SubmitResult result = book.submit(order(4, Side::Buy, 100, 5));

    ASSERT_EQ(result.trades.size(), std::size_t{2});
    expect_trade(result.trades[0], 1, 1, 4, 100, 2);
    expect_trade(result.trades[1], 2, 2, 4, 100, 3);
    EXPECT_FALSE(book.find(OrderId{1}).has_value());
    EXPECT_FALSE(book.find(OrderId{2}).has_value());
    const auto third_maker = book.find(OrderId{3});
    ASSERT_TRUE(third_maker.has_value());
    EXPECT_EQ(third_maker->remaining_quantity, Quantity{4});
    expect_invariants(book);
}

TEST(OrderBookMatchingTest, BuyConsumesAsksInPriceOrder) {
    OrderBook book;
    ASSERT_TRUE(book.submit(order(1, Side::Sell, 100, 1)).accepted());
    ASSERT_TRUE(book.submit(order(2, Side::Sell, 101, 2)).accepted());
    ASSERT_TRUE(book.submit(order(3, Side::Sell, 102, 3)).accepted());

    const SubmitResult result = book.submit(order(4, Side::Buy, 102, 5));

    ASSERT_EQ(result.trades.size(), std::size_t{3});
    expect_trade(result.trades[0], 1, 1, 4, 100, 1);
    expect_trade(result.trades[1], 2, 2, 4, 101, 2);
    expect_trade(result.trades[2], 3, 3, 4, 102, 2);
    const auto last_ask = book.find(OrderId{3});
    ASSERT_TRUE(last_ask.has_value());
    EXPECT_EQ(last_ask->remaining_quantity, Quantity{1});
    expect_invariants(book);
}

TEST(OrderBookMatchingTest, SellConsumesBidsInPriceOrder) {
    OrderBook book;
    ASSERT_TRUE(book.submit(order(1, Side::Buy, 102, 1)).accepted());
    ASSERT_TRUE(book.submit(order(2, Side::Buy, 101, 2)).accepted());
    ASSERT_TRUE(book.submit(order(3, Side::Buy, 100, 3)).accepted());

    const SubmitResult result = book.submit(order(4, Side::Sell, 100, 5));

    ASSERT_EQ(result.trades.size(), std::size_t{3});
    expect_trade(result.trades[0], 1, 1, 4, 102, 1);
    expect_trade(result.trades[1], 2, 2, 4, 101, 2);
    expect_trade(result.trades[2], 3, 3, 4, 100, 2);
    const auto last_bid = book.find(OrderId{3});
    ASSERT_TRUE(last_bid.has_value());
    EXPECT_EQ(last_bid->remaining_quantity, Quantity{1});
    expect_invariants(book);
}

TEST(OrderBookMatchingTest, FilledAndRejectedIdsFollowLifetimeRules) {
    OrderBook book;
    ASSERT_TRUE(book.submit(order(1, Side::Sell, 100, 3)).accepted());
    ASSERT_TRUE(book.submit(order(2, Side::Buy, 100, 3)).accepted());

    EXPECT_EQ(book.submit(order(1, Side::Sell, 101, 1)).rejection_reason,
              RejectionReason::DuplicateOrderId);
    EXPECT_EQ(book.submit(order(2, Side::Buy, 99, 1)).rejection_reason,
              RejectionReason::DuplicateOrderId);
    EXPECT_EQ(book.submit(order(3, Side::Buy, 0, 1)).rejection_reason,
              RejectionReason::InvalidPrice);
    EXPECT_TRUE(book.submit(order(3, Side::Buy, 99, 1)).accepted());
    expect_invariants(book);
}

TEST(OrderBookMatchingTest, TradeIdExhaustionIsRejectedBeforeMutatingTheBook) {
    OrderBook book;
    ASSERT_TRUE(book.submit(order(1, Side::Sell, 100, 1)).accepted());
    ASSERT_TRUE(book.submit(order(2, Side::Sell, 101, 1)).accepted());
    testing::OrderBookTestAccess::set_next_trade_id(
        book,
        TradeId{std::numeric_limits<std::uint64_t>::max()});

    const SubmitResult exhausted = book.submit(order(3, Side::Buy, 101, 2));

    EXPECT_FALSE(exhausted.accepted());
    EXPECT_EQ(exhausted.rejection_reason, RejectionReason::TradeIdExhausted);
    EXPECT_EQ(book.active_order_count(), std::size_t{2});
    ASSERT_TRUE(book.find(OrderId{1}).has_value());
    ASSERT_TRUE(book.find(OrderId{2}).has_value());
    EXPECT_FALSE(book.find(OrderId{3}).has_value());

    const SubmitResult final_valid_trade = book.submit(order(3, Side::Buy, 100, 1));
    ASSERT_TRUE(final_valid_trade.accepted());
    ASSERT_EQ(final_valid_trade.trades.size(), std::size_t{1});
    expect_trade(
        final_valid_trade.trades[0],
        std::numeric_limits<std::uint64_t>::max(),
        1,
        3,
        100,
        1);
    EXPECT_EQ(book.submit(order(4, Side::Buy, 101, 1)).rejection_reason,
              RejectionReason::TradeIdExhausted);
    expect_invariants(book);
}

TEST(OrderBookMatchingTest, ConservationAndInvariantsHoldAfterMixedFills) {
    OrderBook book;
    ASSERT_TRUE(book.submit(order(1, Side::Sell, 100, 2)).accepted());
    ASSERT_TRUE(book.submit(order(2, Side::Sell, 101, 3)).accepted());
    ASSERT_TRUE(book.submit(order(3, Side::Sell, 102, 7)).accepted());

    const Quantity incoming_quantity{9};
    const SubmitResult result = book.submit(NewLimitOrder{
        .id = OrderId{4},
        .side = Side::Buy,
        .limit_price = Price{102},
        .quantity = incoming_quantity,
    });

    ASSERT_TRUE(result.accepted());
    EXPECT_EQ(result.executed_quantity.units + result.resting_quantity.units, incoming_quantity.units);
    ASSERT_EQ(result.trades.size(), std::size_t{3});
    expect_trade(result.trades[0], 1, 1, 4, 100, 2);
    expect_trade(result.trades[1], 2, 2, 4, 101, 3);
    expect_trade(result.trades[2], 3, 3, 4, 102, 4);
    EXPECT_FALSE(book.find(OrderId{1}).has_value());
    EXPECT_FALSE(book.find(OrderId{2}).has_value());
    const auto third_maker = book.find(OrderId{3});
    ASSERT_TRUE(third_maker.has_value());
    EXPECT_EQ(third_maker->remaining_quantity, Quantity{3});
    const auto ask_depth = book.depth(Side::Sell, 10);
    ASSERT_EQ(ask_depth.size(), std::size_t{1});
    EXPECT_EQ(ask_depth[0].price, Price{102});
    EXPECT_EQ(ask_depth[0].total_quantity, Quantity{3});
    expect_invariants(book);
}

} // namespace
} // namespace matching
