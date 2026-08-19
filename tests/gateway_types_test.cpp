#include "bounded_queue.hpp"
#include "matching/gateway_types.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <thread>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

namespace matching {
namespace {

[[nodiscard]] WireEnvelope<ClientMessage> submit_wire_request(
    RequestId request_id,
    std::uint8_t raw_side = static_cast<std::uint8_t>(Side::Buy)) {
    return WireEnvelope<ClientMessage>{
        .request_id = request_id,
        .message = SubmitLimitOrderRequest{
            .order_id = OrderId{1000U + request_id.value},
            .raw_side = raw_side,
            .limit_price = Price{101},
            .quantity = Quantity{10},
        },
    };
}

TEST(GatewayTypesTest, ConnectionIdIsDistinctTransportMetadata) {
    const ConnectionId first{1};
    const ConnectionId second{2};
    EXPECT_LT(first, second);
    EXPECT_NE(first, second);
}

TEST(GatewayTypesTest, RawInvalidSideSurvivesConversionForOrderBookSemanticRejection) {
    const WireEnvelope<ClientMessage> wire = submit_wire_request(RequestId{7}, 0xFFU);
    const EngineRequest engine_request = to_engine_request(ConnectionId{42}, wire);
    ASSERT_TRUE(std::holds_alternative<SubmitEngineRequest>(engine_request));
    const SubmitEngineRequest& submit = std::get<SubmitEngineRequest>(engine_request);
    EXPECT_EQ(submit.connection_id, ConnectionId{42});
    EXPECT_EQ(submit.request_id, RequestId{7});
    EXPECT_EQ(static_cast<std::uint8_t>(submit.order.side), 0xFFU);

    OrderBook order_book;
    const SubmitResult result = order_book.submit(submit.order);
    EXPECT_EQ(result.status, SubmissionStatus::Rejected);
    EXPECT_EQ(result.rejection_reason, RejectionReason::InvalidSide);
    EXPECT_EQ(order_book.active_order_count(), 0U);
}

TEST(GatewayTypesTest, ConvertsSubmitAndCancelRequestsWithoutMixingTransportMetadata) {
    const EngineRequest submit_request = to_engine_request(
        ConnectionId{11}, submit_wire_request(RequestId{12}, static_cast<std::uint8_t>(Side::Sell)));
    ASSERT_TRUE(std::holds_alternative<SubmitEngineRequest>(submit_request));
    const SubmitEngineRequest& submit = std::get<SubmitEngineRequest>(submit_request);
    EXPECT_EQ(submit.connection_id, ConnectionId{11});
    EXPECT_EQ(submit.request_id, RequestId{12});
    EXPECT_EQ(submit.order.id, OrderId{1012});
    EXPECT_EQ(submit.order.side, Side::Sell);

    const WireEnvelope<ClientMessage> wire_cancel{
        .request_id = RequestId{13},
        .message = CancelOrderRequest{.order_id = OrderId{999}},
    };
    const EngineRequest cancel_request = to_engine_request(ConnectionId{14}, wire_cancel);
    ASSERT_TRUE(std::holds_alternative<CancelEngineRequest>(cancel_request));
    const CancelEngineRequest& cancel = std::get<CancelEngineRequest>(cancel_request);
    EXPECT_EQ(cancel.connection_id, ConnectionId{14});
    EXPECT_EQ(cancel.request_id, RequestId{13});
    EXPECT_EQ(cancel.order_id, OrderId{999});
}

TEST(GatewayTypesTest, SummariesExcludeTradesButPreserveWireRelevantResults) {
    SubmitResult submit_result{
        .status = SubmissionStatus::Rejected,
        .rejection_reason = RejectionReason::DuplicateOrderId,
        .executed_quantity = Quantity{3},
        .resting_quantity = Quantity{4},
        .trades = {},
    };
    const SubmitEngineResponse summary = summarize_submit_result(
        ConnectionId{1}, RequestId{2}, submit_result);
    EXPECT_EQ(summary.connection_id, ConnectionId{1});
    EXPECT_EQ(summary.request_id, RequestId{2});
    EXPECT_EQ(summary.status, SubmissionStatus::Rejected);
    EXPECT_EQ(summary.rejection_reason, RejectionReason::DuplicateOrderId);
    EXPECT_EQ(summary.executed_quantity, Quantity{3});
    EXPECT_EQ(summary.resting_quantity, Quantity{4});

    const CancelEngineResponse cancellation = summarize_cancel_result(
        ConnectionId{3}, RequestId{4}, CancelResult{
            .status = CancelStatus::Cancelled,
            .cancelled_quantity = Quantity{5},
        });
    EXPECT_EQ(cancellation.connection_id, ConnectionId{3});
    EXPECT_EQ(cancellation.request_id, RequestId{4});
    EXPECT_EQ(cancellation.status, CancelStatus::Cancelled);
    EXPECT_EQ(cancellation.cancelled_quantity, Quantity{5});
}

TEST(InFlightLimiterTest, EnforcesCapacityAndMoveOnlyReservationsCannotDoubleRelease) {
    InFlightLimiter limiter{2};
    std::optional<InFlightReservation> first = limiter.try_acquire();
    std::optional<InFlightReservation> second = limiter.try_acquire();
    EXPECT_TRUE(first.has_value());
    EXPECT_TRUE(second.has_value());
    EXPECT_FALSE(limiter.try_acquire().has_value());
    EXPECT_EQ(limiter.in_flight(), 2U);
    EXPECT_EQ(limiter.maximum_observed(), 2U);

    InFlightReservation moved = std::move(*first);
    EXPECT_FALSE(first->active());
    EXPECT_TRUE(moved.active());
    moved.release();
    moved.release();
    EXPECT_EQ(limiter.in_flight(), 1U);

    std::optional<InFlightReservation> replacement = limiter.try_acquire();
    EXPECT_TRUE(replacement.has_value());
    EXPECT_EQ(limiter.in_flight(), 2U);
    second.reset();
    replacement.reset();
    EXPECT_EQ(limiter.in_flight(), 0U);
}

TEST(GatewayPipelineTest, TransfersExactlyOneCompletionAndReservationForEveryAdmittedRequest) {
    constexpr std::size_t request_count = 64;
    InFlightLimiter limiter{request_count};
    gateway_detail::BoundedQueue<AdmittedEngineRequest> request_queue{request_count};
    gateway_detail::BoundedQueue<EngineCompletion> response_queue{
        response_queue_capacity_for(limiter.capacity())};

    for (std::size_t index = 0; index < request_count; ++index) {
        std::optional<InFlightReservation> reservation = limiter.try_acquire();
        ASSERT_TRUE(reservation.has_value());
        const RequestId request_id{static_cast<std::uint64_t>(index + 1U)};
        AdmittedEngineRequest admitted{
            .request = to_engine_request(ConnectionId{88}, submit_wire_request(request_id)),
            .completion = std::move(*reservation),
        };
        ASSERT_TRUE(request_queue.try_push(std::move(admitted)));
    }
    EXPECT_EQ(limiter.in_flight(), request_count);
    request_queue.close();

    bool response_publication_failed = false;
    std::jthread worker([&] {
        while (std::optional<AdmittedEngineRequest> admitted = request_queue.wait_pop()) {
            const SubmitEngineRequest& request = std::get<SubmitEngineRequest>(admitted->request);
            EngineResponse response;
            if (request.request_id.value == request_count) {
                response = EngineUnavailableEngineResponse{
                    .connection_id = request.connection_id,
                    .request_id = request.request_id,
                };
            } else {
                response = SubmitEngineResponse{
                    .connection_id = request.connection_id,
                    .request_id = request.request_id,
                    .status = SubmissionStatus::Accepted,
                    .rejection_reason = std::nullopt,
                    .executed_quantity = Quantity{0},
                    .resting_quantity = Quantity{10},
                };
            }
            EngineCompletion completion{
                .response = std::move(response),
                .completion = std::move(admitted->completion),
            };
            if (!response_queue.try_push(std::move(completion))) {
                response_publication_failed = true;
                break;
            }
        }
        response_queue.close();
    });
    worker.join();

    EXPECT_FALSE(response_publication_failed);
    EXPECT_EQ(response_queue.size(), request_count);
    std::vector<bool> completed(request_count, false);
    {
        while (std::optional<EngineCompletion> completion = response_queue.wait_pop()) {
            std::visit(
                [&completed](const auto& response) {
                    ASSERT_EQ(response.connection_id, ConnectionId{88});
                    ASSERT_GE(response.request_id.value, 1U);
                    ASSERT_LE(response.request_id.value, completed.size());
                    const std::size_t index = static_cast<std::size_t>(response.request_id.value - 1U);
                    EXPECT_FALSE(completed[index]);
                    completed[index] = true;
                },
                completion->response);
        }
    }
    for (const bool was_completed : completed) {
        EXPECT_TRUE(was_completed);
    }
    EXPECT_EQ(limiter.in_flight(), 0U);
    EXPECT_LE(limiter.maximum_observed(), limiter.capacity());
}

} // namespace
} // namespace matching
