#include "matching/gateway_types.hpp"

#include <exception>
#include <type_traits>
#include <utility>

namespace matching {
namespace {

static_assert(std::is_same_v<std::underlying_type_t<Side>, std::uint8_t>);

} // namespace

Side side_from_wire_raw(std::uint8_t raw_side) noexcept {
    return static_cast<Side>(raw_side);
}

NewLimitOrder to_domain_order(const SubmitLimitOrderRequest& request) noexcept {
    return NewLimitOrder{
        .id = request.order_id,
        .side = side_from_wire_raw(request.raw_side),
        .limit_price = request.limit_price,
        .quantity = request.quantity,
    };
}

EngineRequest to_engine_request(
    ConnectionId connection_id,
    const WireEnvelope<ClientMessage>& request) noexcept {
    return std::visit(
        [connection_id, request_id = request.request_id](const auto& message) -> EngineRequest {
            using Message = std::decay_t<decltype(message)>;
            if constexpr (std::is_same_v<Message, SubmitLimitOrderRequest>) {
                return SubmitEngineRequest{
                    .connection_id = connection_id,
                    .request_id = request_id,
                    .order = to_domain_order(message),
                };
            } else {
                return CancelEngineRequest{
                    .connection_id = connection_id,
                    .request_id = request_id,
                    .order_id = message.order_id,
                };
            }
        },
        request.message);
}

SubmitEngineResponse summarize_submit_result(
    ConnectionId connection_id,
    RequestId request_id,
    const SubmitResult& result) noexcept {
    return SubmitEngineResponse{
        .connection_id = connection_id,
        .request_id = request_id,
        .status = result.status,
        .rejection_reason = result.rejection_reason,
        .executed_quantity = result.executed_quantity,
        .resting_quantity = result.resting_quantity,
    };
}

CancelEngineResponse summarize_cancel_result(
    ConnectionId connection_id,
    RequestId request_id,
    const CancelResult& result) noexcept {
    return CancelEngineResponse{
        .connection_id = connection_id,
        .request_id = request_id,
        .status = result.status,
        .cancelled_quantity = result.cancelled_quantity,
    };
}

InFlightReservation::InFlightReservation(InFlightReservation&& other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)) {}

InFlightReservation& InFlightReservation::operator=(InFlightReservation&& other) noexcept {
    if (this != &other) {
        release();
        owner_ = std::exchange(other.owner_, nullptr);
    }
    return *this;
}

InFlightReservation::~InFlightReservation() {
    release();
}

void InFlightReservation::release() noexcept {
    if (owner_ != nullptr) {
        owner_->release_one();
        owner_ = nullptr;
    }
}

bool InFlightReservation::active() const noexcept {
    return owner_ != nullptr;
}

std::optional<InFlightReservation> InFlightLimiter::try_acquire() noexcept {
    std::lock_guard lock(mutex_);
    if (in_flight_ == capacity_) {
        return std::nullopt;
    }
    ++in_flight_;
    if (in_flight_ > maximum_observed_) {
        maximum_observed_ = in_flight_;
    }
    return InFlightReservation{this};
}

std::size_t InFlightLimiter::in_flight() const noexcept {
    std::lock_guard lock(mutex_);
    return in_flight_;
}

std::size_t InFlightLimiter::capacity() const noexcept {
    return capacity_;
}

std::size_t InFlightLimiter::maximum_observed() const noexcept {
    std::lock_guard lock(mutex_);
    return maximum_observed_;
}

void InFlightLimiter::release_one() noexcept {
    std::lock_guard lock(mutex_);
    if (in_flight_ == 0) {
        std::terminate();
    }
    --in_flight_;
}

} // namespace matching
