#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <variant>

#include "matching/order_book.hpp"
#include "matching/wire_protocol.hpp"

namespace matching {

// Server-assigned transport identity. It is not transmitted or persisted.
struct ConnectionId {
    std::uint64_t value{};

    constexpr auto operator<=>(const ConnectionId&) const = default;
};

// Converts every possible raw wire octet to Side without rejecting it. Side has
// std::uint8_t as its fixed underlying type, so this conversion is defined for
// every raw wire value. OrderBook remains responsible for semantic validation.
[[nodiscard]] Side side_from_wire_raw(std::uint8_t raw_side) noexcept;
[[nodiscard]] NewLimitOrder to_domain_order(const SubmitLimitOrderRequest& request) noexcept;

struct SubmitEngineRequest {
    ConnectionId connection_id;
    RequestId request_id;
    NewLimitOrder order;
};

struct CancelEngineRequest {
    ConnectionId connection_id;
    RequestId request_id;
    OrderId order_id;
};

using EngineRequest = std::variant<SubmitEngineRequest, CancelEngineRequest>;

// Converts an already decoded, structurally valid wire request into the typed
// request that a future engine worker will execute through DurableEngine.
[[nodiscard]] EngineRequest to_engine_request(
    ConnectionId connection_id,
    const WireEnvelope<ClientMessage>& request) noexcept;

// v1 network responses intentionally carry only the bounded submit summary;
// trade vectors stay inside the matching engine API.
struct SubmitEngineResponse {
    ConnectionId connection_id;
    RequestId request_id;
    SubmissionStatus status;
    std::optional<RejectionReason> rejection_reason{};
    Quantity executed_quantity{};
    Quantity resting_quantity{};
};

struct CancelEngineResponse {
    ConnectionId connection_id;
    RequestId request_id;
    CancelStatus status;
    Quantity cancelled_quantity{};
};

struct EngineUnavailableEngineResponse {
    ConnectionId connection_id;
    RequestId request_id;
};

using EngineResponse = std::variant<
    SubmitEngineResponse,
    CancelEngineResponse,
    EngineUnavailableEngineResponse>;

[[nodiscard]] SubmitEngineResponse summarize_submit_result(
    ConnectionId connection_id,
    RequestId request_id,
    const SubmitResult& result) noexcept;
[[nodiscard]] CancelEngineResponse summarize_cancel_result(
    ConnectionId connection_id,
    RequestId request_id,
    const CancelResult& result) noexcept;

class InFlightLimiter;

// A move-only completion slot. It is acquired before admission, transferred
// request -> response, and released when the gateway has drained that response.
class InFlightReservation {
public:
    InFlightReservation() = default;
    InFlightReservation(const InFlightReservation&) = delete;
    InFlightReservation& operator=(const InFlightReservation&) = delete;
    InFlightReservation(InFlightReservation&& other) noexcept;
    InFlightReservation& operator=(InFlightReservation&& other) noexcept;
    ~InFlightReservation();

    void release() noexcept;
    [[nodiscard]] bool active() const noexcept;

private:
    friend class InFlightLimiter;

    explicit InFlightReservation(InFlightLimiter* owner) noexcept : owner_(owner) {}

    InFlightLimiter* owner_{};
};

class InFlightLimiter {
public:
    explicit InFlightLimiter(std::size_t capacity) noexcept : capacity_(capacity) {}

    InFlightLimiter(const InFlightLimiter&) = delete;
    InFlightLimiter& operator=(const InFlightLimiter&) = delete;

    [[nodiscard]] std::optional<InFlightReservation> try_acquire() noexcept;
    [[nodiscard]] std::size_t in_flight() const noexcept;
    [[nodiscard]] std::size_t capacity() const noexcept;
    [[nodiscard]] std::size_t maximum_observed() const noexcept;

private:
    friend class InFlightReservation;

    void release_one() noexcept;

    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::size_t in_flight_{};
    std::size_t maximum_observed_{};
};

struct AdmittedEngineRequest {
    EngineRequest request;
    InFlightReservation completion;
};

// Shutdown closes the request queue first. The engine drains these objects,
// moves each reservation here with exactly one terminal response, then closes
// the response queue. The gateway releases the reservation after draining it.
struct EngineCompletion {
    EngineResponse response;
    InFlightReservation completion;
};

// A response queue must be constructed with this capacity (or greater). With
// at most N reservations, moving one executing request to its response can
// never encounter a full N-slot response queue.
[[nodiscard]] constexpr std::size_t response_queue_capacity_for(
    std::size_t maximum_in_flight) noexcept {
    return maximum_in_flight;
}

} // namespace matching
