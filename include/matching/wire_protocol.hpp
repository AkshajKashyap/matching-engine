#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <variant>
#include <vector>

#include "matching/order.hpp"
#include "matching/order_book.hpp"

namespace matching {

// A client supplied transport correlation value. It is intentionally separate
// from OrderId, TradeId, and the durable journal sequence.
struct RequestId {
    std::uint64_t value{};

    constexpr auto operator<=>(const RequestId&) const = default;
};

// The numeric value 0x4D455847 is encoded in big-endian order as ASCII MEXG.
inline constexpr std::uint32_t kWireProtocolMagic = 0x4D455847U;
inline constexpr std::uint16_t kWireProtocolVersion = 1;
inline constexpr std::size_t kWireFrameHeaderSize = 20;
inline constexpr std::size_t kWireMaximumPayloadLength = 4096;
inline constexpr std::size_t kSubmitLimitOrderRequestPayloadSize = 25;
inline constexpr std::size_t kCancelOrderRequestPayloadSize = 8;
inline constexpr std::size_t kSubmitResultResponsePayloadSize = 18;
inline constexpr std::size_t kCancelResultResponsePayloadSize = 9;
inline constexpr std::size_t kProtocolErrorResponsePayloadSize = 2;
inline constexpr std::size_t kEngineUnavailableResponsePayloadSize = 1;

// Request values occupy 1..2 and response values occupy 101..104.
enum class WireMessageType : std::uint16_t {
    SubmitLimitOrder = 1,
    CancelOrder = 2,
    SubmitResult = 101,
    CancelResult = 102,
    ProtocolError = 103,
    EngineUnavailable = 104,
};

// These values are protocol values, not the representation of matching enums.
enum class WireRejectionCode : std::uint8_t {
    None = 0,
    InvalidOrderId = 1,
    InvalidSide = 2,
    InvalidPrice = 3,
    InvalidQuantity = 4,
    DuplicateOrderId = 5,
    QuantityOverflow = 6,
    SequenceExhausted = 7,
    TradeIdExhausted = 8,
};

enum class WireSubmitStatus : std::uint8_t {
    Accepted = 1,
    Rejected = 2,
};

enum class WireCancelStatus : std::uint8_t {
    Cancelled = 1,
    NotFound = 2,
};

// Codes suitable for a bounded ProtocolError response payload.
enum class WireProtocolErrorCode : std::uint16_t {
    BadMagic = 1,
    UnsupportedVersion = 2,
    UnsupportedMessageType = 3,
    PayloadTooLarge = 4,
    InvalidPayloadLength = 5,
    MalformedPayload = 6,
    BufferLimitExceeded = 7,
};

enum class WireEngineUnavailableCode : std::uint8_t {
    Unavailable = 1,
};

// Decoder/parser-only errors include conditions that are not useful to send to
// a peer, such as needing more stream bytes or trailing input for an exact
// single-frame decoder.
enum class WireCodecError : std::uint8_t {
    TruncatedInput,
    UnexpectedTrailingBytes,
    BadMagic,
    UnsupportedVersion,
    UnsupportedMessageType,
    PayloadTooLarge,
    InvalidPayloadLength,
    MalformedPayload,
    BufferLimitExceeded,
};

struct SubmitLimitOrderRequest {
    OrderId order_id;
    // Retained as an octet. The transport must not construct an invalid Side
    // enum merely to preserve a malformed-but-framed client request.
    std::uint8_t raw_side{};
    Price limit_price;
    Quantity quantity;
};

struct CancelOrderRequest {
    OrderId order_id;
};

using ClientMessage = std::variant<SubmitLimitOrderRequest, CancelOrderRequest>;

struct SubmitResultResponse {
    WireSubmitStatus status;
    WireRejectionCode rejection_code;
    Quantity executed_quantity;
    Quantity resting_quantity;
};

struct CancelResultResponse {
    WireCancelStatus status;
    Quantity cancelled_quantity;
};

struct ProtocolErrorResponse {
    WireProtocolErrorCode code;
};

struct EngineUnavailableResponse {
    WireEngineUnavailableCode code{WireEngineUnavailableCode::Unavailable};
};

using ServerMessage = std::variant<
    SubmitResultResponse,
    CancelResultResponse,
    ProtocolErrorResponse,
    EngineUnavailableResponse>;

template <typename Message>
struct WireEnvelope {
    RequestId request_id;
    Message message;
};

struct DecodedClientFrame {
    WireEnvelope<ClientMessage> envelope;
    std::size_t bytes_consumed{};
};

struct DecodedServerFrame {
    WireEnvelope<ServerMessage> envelope;
    std::size_t bytes_consumed{};
};

using EncodedWireFrame = std::variant<std::vector<std::byte>, WireCodecError>;
using DecodedClientFrameResult = std::variant<DecodedClientFrame, WireCodecError>;
using DecodedServerFrameResult = std::variant<DecodedServerFrame, WireCodecError>;

[[nodiscard]] std::optional<WireRejectionCode> to_wire_rejection_code(
    RejectionReason reason) noexcept;
[[nodiscard]] std::optional<RejectionReason> to_domain_rejection_reason(
    WireRejectionCode code) noexcept;
[[nodiscard]] std::optional<WireCancelStatus> to_wire_cancel_status(
    CancelStatus status) noexcept;
[[nodiscard]] std::optional<CancelStatus> to_domain_cancel_status(
    WireCancelStatus status) noexcept;
[[nodiscard]] std::optional<WireProtocolErrorCode> to_wire_protocol_error_code(
    WireCodecError error) noexcept;

[[nodiscard]] EncodedWireFrame encode_client_frame(
    const WireEnvelope<ClientMessage>& envelope);
[[nodiscard]] EncodedWireFrame encode_server_frame(
    const WireEnvelope<ServerMessage>& envelope);

// Prefix decoders consume one complete frame and deliberately leave a possible
// following frame for a stream parser. TruncatedInput means more bytes may make
// the current frame valid; every other error is terminal for the frame.
[[nodiscard]] DecodedClientFrameResult decode_client_frame_prefix(
    std::span<const std::byte> bytes);
[[nodiscard]] DecodedServerFrameResult decode_server_frame_prefix(
    std::span<const std::byte> bytes);

// Exact decoders reject any bytes following the one frame.
[[nodiscard]] DecodedClientFrameResult decode_client_frame(std::span<const std::byte> bytes);
[[nodiscard]] DecodedServerFrameResult decode_server_frame(std::span<const std::byte> bytes);

struct ClientParseFeedResult {
    std::vector<WireEnvelope<ClientMessage>> messages;
    std::optional<WireCodecError> error{};
};

// Stateful framing for one future TCP connection. It has no socket knowledge.
class ClientStreamParser {
public:
    explicit ClientStreamParser(std::size_t maximum_buffered_bytes = 64U * 1024U);

    [[nodiscard]] ClientParseFeedResult feed(std::span<const std::byte> bytes);

    [[nodiscard]] bool failed() const noexcept;
    [[nodiscard]] std::optional<WireCodecError> failure() const noexcept;
    [[nodiscard]] bool has_incomplete_frame() const noexcept;
    [[nodiscard]] std::size_t buffered_size() const noexcept;

private:
    void compact_if_needed();
    void process_available(ClientParseFeedResult& result);
    void fail(ClientParseFeedResult& result, WireCodecError error);

    std::vector<std::byte> buffer_;
    std::size_t read_cursor_{};
    std::size_t maximum_buffered_bytes_{};
    std::optional<WireCodecError> failure_{};
};

} // namespace matching
