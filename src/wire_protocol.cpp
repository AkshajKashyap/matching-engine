#include "matching/wire_protocol.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>

namespace matching {
namespace {

struct FrameHeader {
    WireMessageType type;
    std::uint32_t payload_length{};
    RequestId request_id;
};

using DecodedHeaderResult = std::variant<FrameHeader, WireCodecError>;

[[nodiscard]] std::uint8_t byte_value(std::byte value) noexcept {
    return std::to_integer<std::uint8_t>(value);
}

void append_u8(std::vector<std::byte>& bytes, std::uint8_t value) {
    bytes.push_back(static_cast<std::byte>(value));
}

void append_u16(std::vector<std::byte>& bytes, std::uint16_t value) {
    append_u8(bytes, static_cast<std::uint8_t>(value >> 8U));
    append_u8(bytes, static_cast<std::uint8_t>(value));
}

void append_u32(std::vector<std::byte>& bytes, std::uint32_t value) {
    for (std::uint32_t shift = 24; ; shift -= 8) {
        append_u8(bytes, static_cast<std::uint8_t>(value >> shift));
        if (shift == 0) {
            return;
        }
    }
}

void append_u64(std::vector<std::byte>& bytes, std::uint64_t value) {
    for (std::uint32_t shift = 56; ; shift -= 8) {
        append_u8(bytes, static_cast<std::uint8_t>(value >> shift));
        if (shift == 0) {
            return;
        }
    }
}

[[nodiscard]] std::uint16_t read_u16(std::span<const std::byte> bytes, std::size_t offset) noexcept {
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(byte_value(bytes[offset])) << 8U) |
        static_cast<std::uint16_t>(byte_value(bytes[offset + 1U])));
}

[[nodiscard]] std::uint32_t read_u32(std::span<const std::byte> bytes, std::size_t offset) noexcept {
    std::uint32_t value = 0;
    for (std::size_t index = 0; index < 4; ++index) {
        value = static_cast<std::uint32_t>((value << 8U) | byte_value(bytes[offset + index]));
    }
    return value;
}

[[nodiscard]] std::uint64_t read_u64(std::span<const std::byte> bytes, std::size_t offset) noexcept {
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < 8; ++index) {
        value = (value << 8U) | byte_value(bytes[offset + index]);
    }
    return value;
}

[[nodiscard]] std::optional<WireMessageType> decode_message_type(std::uint16_t value) noexcept {
    switch (value) {
    case static_cast<std::uint16_t>(WireMessageType::SubmitLimitOrder):
        return WireMessageType::SubmitLimitOrder;
    case static_cast<std::uint16_t>(WireMessageType::CancelOrder):
        return WireMessageType::CancelOrder;
    case static_cast<std::uint16_t>(WireMessageType::SubmitResult):
        return WireMessageType::SubmitResult;
    case static_cast<std::uint16_t>(WireMessageType::CancelResult):
        return WireMessageType::CancelResult;
    case static_cast<std::uint16_t>(WireMessageType::ProtocolError):
        return WireMessageType::ProtocolError;
    case static_cast<std::uint16_t>(WireMessageType::EngineUnavailable):
        return WireMessageType::EngineUnavailable;
    }
    return std::nullopt;
}

[[nodiscard]] bool is_client_message_type(WireMessageType type) noexcept {
    return type == WireMessageType::SubmitLimitOrder || type == WireMessageType::CancelOrder;
}

[[nodiscard]] bool is_server_message_type(WireMessageType type) noexcept {
    return type == WireMessageType::SubmitResult || type == WireMessageType::CancelResult ||
           type == WireMessageType::ProtocolError || type == WireMessageType::EngineUnavailable;
}

[[nodiscard]] std::optional<std::size_t> expected_payload_length(WireMessageType type) noexcept {
    switch (type) {
    case WireMessageType::SubmitLimitOrder:
        return kSubmitLimitOrderRequestPayloadSize;
    case WireMessageType::CancelOrder:
        return kCancelOrderRequestPayloadSize;
    case WireMessageType::SubmitResult:
        return kSubmitResultResponsePayloadSize;
    case WireMessageType::CancelResult:
        return kCancelResultResponsePayloadSize;
    case WireMessageType::ProtocolError:
        return kProtocolErrorResponsePayloadSize;
    case WireMessageType::EngineUnavailable:
        return kEngineUnavailableResponsePayloadSize;
    }
    return std::nullopt;
}

[[nodiscard]] DecodedHeaderResult decode_header(std::span<const std::byte> bytes) {
    if (bytes.size() < 4U) {
        return WireCodecError::TruncatedInput;
    }
    if (read_u32(bytes, 0) != kWireProtocolMagic) {
        return WireCodecError::BadMagic;
    }
    if (bytes.size() < 6U) {
        return WireCodecError::TruncatedInput;
    }
    if (read_u16(bytes, 4) != kWireProtocolVersion) {
        return WireCodecError::UnsupportedVersion;
    }
    if (bytes.size() < 8U) {
        return WireCodecError::TruncatedInput;
    }
    const auto type = decode_message_type(read_u16(bytes, 6));
    if (!type.has_value()) {
        return WireCodecError::UnsupportedMessageType;
    }
    if (bytes.size() < 12U) {
        return WireCodecError::TruncatedInput;
    }
    const std::uint32_t payload_length = read_u32(bytes, 8);
    if (payload_length > kWireMaximumPayloadLength) {
        return WireCodecError::PayloadTooLarge;
    }
    const auto expected_length = expected_payload_length(*type);
    if (!expected_length.has_value() || payload_length != *expected_length) {
        return WireCodecError::InvalidPayloadLength;
    }
    if (bytes.size() < kWireFrameHeaderSize) {
        return WireCodecError::TruncatedInput;
    }
    return FrameHeader{
        .type = *type,
        .payload_length = payload_length,
        .request_id = RequestId{read_u64(bytes, 12)},
    };
}

[[nodiscard]] bool has_complete_frame(
    std::span<const std::byte> bytes,
    const FrameHeader& header) noexcept {
    return bytes.size() >= kWireFrameHeaderSize + header.payload_length;
}

[[nodiscard]] bool is_known_rejection_code(WireRejectionCode code) noexcept {
    switch (code) {
    case WireRejectionCode::None:
    case WireRejectionCode::InvalidOrderId:
    case WireRejectionCode::InvalidSide:
    case WireRejectionCode::InvalidPrice:
    case WireRejectionCode::InvalidQuantity:
    case WireRejectionCode::DuplicateOrderId:
    case WireRejectionCode::QuantityOverflow:
    case WireRejectionCode::SequenceExhausted:
    case WireRejectionCode::TradeIdExhausted:
        return true;
    }
    return false;
}

[[nodiscard]] std::optional<WireRejectionCode> decode_rejection_code(std::uint8_t value) noexcept {
    const auto code = static_cast<WireRejectionCode>(value);
    return is_known_rejection_code(code) ? std::optional<WireRejectionCode>{code} : std::nullopt;
}

[[nodiscard]] std::optional<WireSubmitStatus> decode_submit_status(std::uint8_t value) noexcept {
    switch (value) {
    case static_cast<std::uint8_t>(WireSubmitStatus::Accepted):
        return WireSubmitStatus::Accepted;
    case static_cast<std::uint8_t>(WireSubmitStatus::Rejected):
        return WireSubmitStatus::Rejected;
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<WireCancelStatus> decode_cancel_status(std::uint8_t value) noexcept {
    switch (value) {
    case static_cast<std::uint8_t>(WireCancelStatus::Cancelled):
        return WireCancelStatus::Cancelled;
    case static_cast<std::uint8_t>(WireCancelStatus::NotFound):
        return WireCancelStatus::NotFound;
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<WireProtocolErrorCode> decode_protocol_error_code(
    std::uint16_t value) noexcept {
    switch (value) {
    case static_cast<std::uint16_t>(WireProtocolErrorCode::BadMagic):
        return WireProtocolErrorCode::BadMagic;
    case static_cast<std::uint16_t>(WireProtocolErrorCode::UnsupportedVersion):
        return WireProtocolErrorCode::UnsupportedVersion;
    case static_cast<std::uint16_t>(WireProtocolErrorCode::UnsupportedMessageType):
        return WireProtocolErrorCode::UnsupportedMessageType;
    case static_cast<std::uint16_t>(WireProtocolErrorCode::PayloadTooLarge):
        return WireProtocolErrorCode::PayloadTooLarge;
    case static_cast<std::uint16_t>(WireProtocolErrorCode::InvalidPayloadLength):
        return WireProtocolErrorCode::InvalidPayloadLength;
    case static_cast<std::uint16_t>(WireProtocolErrorCode::MalformedPayload):
        return WireProtocolErrorCode::MalformedPayload;
    case static_cast<std::uint16_t>(WireProtocolErrorCode::BufferLimitExceeded):
        return WireProtocolErrorCode::BufferLimitExceeded;
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<WireEngineUnavailableCode> decode_engine_unavailable_code(
    std::uint8_t value) noexcept {
    if (value == static_cast<std::uint8_t>(WireEngineUnavailableCode::Unavailable)) {
        return WireEngineUnavailableCode::Unavailable;
    }
    return std::nullopt;
}

[[nodiscard]] bool valid_submit_result(const SubmitResultResponse& response) noexcept {
    if (!is_known_rejection_code(response.rejection_code)) {
        return false;
    }
    return (response.status == WireSubmitStatus::Accepted &&
            response.rejection_code == WireRejectionCode::None) ||
           (response.status == WireSubmitStatus::Rejected &&
            response.rejection_code != WireRejectionCode::None);
}

[[nodiscard]] bool valid_cancel_result(const CancelResultResponse& response) noexcept {
    return response.status == WireCancelStatus::Cancelled || response.status == WireCancelStatus::NotFound;
}

[[nodiscard]] std::vector<std::byte> encode_header(
    WireMessageType type,
    std::size_t payload_length,
    RequestId request_id) {
    std::vector<std::byte> bytes;
    bytes.reserve(kWireFrameHeaderSize + payload_length);
    append_u32(bytes, kWireProtocolMagic);
    append_u16(bytes, kWireProtocolVersion);
    append_u16(bytes, static_cast<std::uint16_t>(type));
    append_u32(bytes, static_cast<std::uint32_t>(payload_length));
    append_u64(bytes, request_id.value);
    return bytes;
}

template <typename DecodedFrame>
[[nodiscard]] std::variant<DecodedFrame, WireCodecError> require_exact_frame(
    std::variant<DecodedFrame, WireCodecError> result,
    std::size_t input_size) {
    if (const auto* error = std::get_if<WireCodecError>(&result)) {
        return *error;
    }
    if (std::get<DecodedFrame>(result).bytes_consumed != input_size) {
        return WireCodecError::UnexpectedTrailingBytes;
    }
    return std::get<DecodedFrame>(std::move(result));
}

} // namespace

std::optional<WireRejectionCode> to_wire_rejection_code(RejectionReason reason) noexcept {
    switch (reason) {
    case RejectionReason::InvalidOrderId:
        return WireRejectionCode::InvalidOrderId;
    case RejectionReason::InvalidSide:
        return WireRejectionCode::InvalidSide;
    case RejectionReason::InvalidPrice:
        return WireRejectionCode::InvalidPrice;
    case RejectionReason::InvalidQuantity:
        return WireRejectionCode::InvalidQuantity;
    case RejectionReason::DuplicateOrderId:
        return WireRejectionCode::DuplicateOrderId;
    case RejectionReason::QuantityOverflow:
        return WireRejectionCode::QuantityOverflow;
    case RejectionReason::SequenceExhausted:
        return WireRejectionCode::SequenceExhausted;
    case RejectionReason::TradeIdExhausted:
        return WireRejectionCode::TradeIdExhausted;
    }
    return std::nullopt;
}

std::optional<RejectionReason> to_domain_rejection_reason(WireRejectionCode code) noexcept {
    switch (code) {
    case WireRejectionCode::InvalidOrderId:
        return RejectionReason::InvalidOrderId;
    case WireRejectionCode::InvalidSide:
        return RejectionReason::InvalidSide;
    case WireRejectionCode::InvalidPrice:
        return RejectionReason::InvalidPrice;
    case WireRejectionCode::InvalidQuantity:
        return RejectionReason::InvalidQuantity;
    case WireRejectionCode::DuplicateOrderId:
        return RejectionReason::DuplicateOrderId;
    case WireRejectionCode::QuantityOverflow:
        return RejectionReason::QuantityOverflow;
    case WireRejectionCode::SequenceExhausted:
        return RejectionReason::SequenceExhausted;
    case WireRejectionCode::TradeIdExhausted:
        return RejectionReason::TradeIdExhausted;
    case WireRejectionCode::None:
        return std::nullopt;
    }
    return std::nullopt;
}

std::optional<WireCancelStatus> to_wire_cancel_status(CancelStatus status) noexcept {
    switch (status) {
    case CancelStatus::Cancelled:
        return WireCancelStatus::Cancelled;
    case CancelStatus::NotFound:
        return WireCancelStatus::NotFound;
    }
    return std::nullopt;
}

std::optional<CancelStatus> to_domain_cancel_status(WireCancelStatus status) noexcept {
    switch (status) {
    case WireCancelStatus::Cancelled:
        return CancelStatus::Cancelled;
    case WireCancelStatus::NotFound:
        return CancelStatus::NotFound;
    }
    return std::nullopt;
}

std::optional<WireProtocolErrorCode> to_wire_protocol_error_code(WireCodecError error) noexcept {
    switch (error) {
    case WireCodecError::BadMagic:
        return WireProtocolErrorCode::BadMagic;
    case WireCodecError::UnsupportedVersion:
        return WireProtocolErrorCode::UnsupportedVersion;
    case WireCodecError::UnsupportedMessageType:
        return WireProtocolErrorCode::UnsupportedMessageType;
    case WireCodecError::PayloadTooLarge:
        return WireProtocolErrorCode::PayloadTooLarge;
    case WireCodecError::InvalidPayloadLength:
        return WireProtocolErrorCode::InvalidPayloadLength;
    case WireCodecError::MalformedPayload:
        return WireProtocolErrorCode::MalformedPayload;
    case WireCodecError::BufferLimitExceeded:
        return WireProtocolErrorCode::BufferLimitExceeded;
    case WireCodecError::TruncatedInput:
    case WireCodecError::UnexpectedTrailingBytes:
        return std::nullopt;
    }
    return std::nullopt;
}

EncodedWireFrame encode_client_frame(const WireEnvelope<ClientMessage>& envelope) {
    return std::visit(
        [&envelope](const auto& message) -> EncodedWireFrame {
            using Message = std::decay_t<decltype(message)>;
            if constexpr (std::is_same_v<Message, SubmitLimitOrderRequest>) {
                std::vector<std::byte> bytes = encode_header(
                    WireMessageType::SubmitLimitOrder,
                    kSubmitLimitOrderRequestPayloadSize,
                    envelope.request_id);
                append_u64(bytes, message.order_id.value);
                append_u8(bytes, message.raw_side);
                append_u64(bytes, message.limit_price.ticks);
                append_u64(bytes, message.quantity.units);
                return bytes;
            } else {
                std::vector<std::byte> bytes = encode_header(
                    WireMessageType::CancelOrder,
                    kCancelOrderRequestPayloadSize,
                    envelope.request_id);
                append_u64(bytes, message.order_id.value);
                return bytes;
            }
        },
        envelope.message);
}

EncodedWireFrame encode_server_frame(const WireEnvelope<ServerMessage>& envelope) {
    return std::visit(
        [&envelope](const auto& message) -> EncodedWireFrame {
            using Message = std::decay_t<decltype(message)>;
            if constexpr (std::is_same_v<Message, SubmitResultResponse>) {
                if (!valid_submit_result(message)) {
                    return WireCodecError::MalformedPayload;
                }
                std::vector<std::byte> bytes = encode_header(
                    WireMessageType::SubmitResult,
                    kSubmitResultResponsePayloadSize,
                    envelope.request_id);
                append_u8(bytes, static_cast<std::uint8_t>(message.status));
                append_u8(bytes, static_cast<std::uint8_t>(message.rejection_code));
                append_u64(bytes, message.executed_quantity.units);
                append_u64(bytes, message.resting_quantity.units);
                return bytes;
            } else if constexpr (std::is_same_v<Message, CancelResultResponse>) {
                if (!valid_cancel_result(message)) {
                    return WireCodecError::MalformedPayload;
                }
                std::vector<std::byte> bytes = encode_header(
                    WireMessageType::CancelResult,
                    kCancelResultResponsePayloadSize,
                    envelope.request_id);
                append_u8(bytes, static_cast<std::uint8_t>(message.status));
                append_u64(bytes, message.cancelled_quantity.units);
                return bytes;
            } else if constexpr (std::is_same_v<Message, ProtocolErrorResponse>) {
                if (!decode_protocol_error_code(static_cast<std::uint16_t>(message.code)).has_value()) {
                    return WireCodecError::MalformedPayload;
                }
                std::vector<std::byte> bytes = encode_header(
                    WireMessageType::ProtocolError,
                    kProtocolErrorResponsePayloadSize,
                    envelope.request_id);
                append_u16(bytes, static_cast<std::uint16_t>(message.code));
                return bytes;
            } else {
                if (message.code != WireEngineUnavailableCode::Unavailable) {
                    return WireCodecError::MalformedPayload;
                }
                std::vector<std::byte> bytes = encode_header(
                    WireMessageType::EngineUnavailable,
                    kEngineUnavailableResponsePayloadSize,
                    envelope.request_id);
                append_u8(bytes, static_cast<std::uint8_t>(message.code));
                return bytes;
            }
        },
        envelope.message);
}

DecodedClientFrameResult decode_client_frame_prefix(std::span<const std::byte> bytes) {
    const DecodedHeaderResult header_result = decode_header(bytes);
    if (const auto* error = std::get_if<WireCodecError>(&header_result)) {
        return *error;
    }
    const FrameHeader& header = std::get<FrameHeader>(header_result);
    if (!is_client_message_type(header.type)) {
        return WireCodecError::UnsupportedMessageType;
    }
    if (!has_complete_frame(bytes, header)) {
        return WireCodecError::TruncatedInput;
    }

    const std::span<const std::byte> payload = bytes.subspan(
        kWireFrameHeaderSize,
        static_cast<std::size_t>(header.payload_length));
    ClientMessage message;
    if (header.type == WireMessageType::SubmitLimitOrder) {
        message = SubmitLimitOrderRequest{
            .order_id = OrderId{read_u64(payload, 0)},
            .raw_side = byte_value(payload[8]),
            .limit_price = Price{read_u64(payload, 9)},
            .quantity = Quantity{read_u64(payload, 17)},
        };
    } else {
        message = CancelOrderRequest{.order_id = OrderId{read_u64(payload, 0)}};
    }
    return DecodedClientFrame{
        .envelope = WireEnvelope<ClientMessage>{
            .request_id = header.request_id,
            .message = std::move(message),
        },
        .bytes_consumed = kWireFrameHeaderSize + header.payload_length,
    };
}

DecodedServerFrameResult decode_server_frame_prefix(std::span<const std::byte> bytes) {
    const DecodedHeaderResult header_result = decode_header(bytes);
    if (const auto* error = std::get_if<WireCodecError>(&header_result)) {
        return *error;
    }
    const FrameHeader& header = std::get<FrameHeader>(header_result);
    if (!is_server_message_type(header.type)) {
        return WireCodecError::UnsupportedMessageType;
    }
    if (!has_complete_frame(bytes, header)) {
        return WireCodecError::TruncatedInput;
    }

    const std::span<const std::byte> payload = bytes.subspan(
        kWireFrameHeaderSize,
        static_cast<std::size_t>(header.payload_length));
    ServerMessage message;
    switch (header.type) {
    case WireMessageType::SubmitResult: {
        const auto status = decode_submit_status(byte_value(payload[0]));
        const auto rejection_code = decode_rejection_code(byte_value(payload[1]));
        if (!status.has_value() || !rejection_code.has_value()) {
            return WireCodecError::MalformedPayload;
        }
        const SubmitResultResponse response{
            .status = *status,
            .rejection_code = *rejection_code,
            .executed_quantity = Quantity{read_u64(payload, 2)},
            .resting_quantity = Quantity{read_u64(payload, 10)},
        };
        if (!valid_submit_result(response)) {
            return WireCodecError::MalformedPayload;
        }
        message = response;
        break;
    }
    case WireMessageType::CancelResult: {
        const auto status = decode_cancel_status(byte_value(payload[0]));
        if (!status.has_value()) {
            return WireCodecError::MalformedPayload;
        }
        const CancelResultResponse response{
            .status = *status,
            .cancelled_quantity = Quantity{read_u64(payload, 1)},
        };
        if (!valid_cancel_result(response)) {
            return WireCodecError::MalformedPayload;
        }
        message = response;
        break;
    }
    case WireMessageType::ProtocolError: {
        const auto code = decode_protocol_error_code(read_u16(payload, 0));
        if (!code.has_value()) {
            return WireCodecError::MalformedPayload;
        }
        message = ProtocolErrorResponse{.code = *code};
        break;
    }
    case WireMessageType::EngineUnavailable: {
        const auto code = decode_engine_unavailable_code(byte_value(payload[0]));
        if (!code.has_value()) {
            return WireCodecError::MalformedPayload;
        }
        message = EngineUnavailableResponse{.code = *code};
        break;
    }
    case WireMessageType::SubmitLimitOrder:
    case WireMessageType::CancelOrder:
        return WireCodecError::UnsupportedMessageType;
    }
    return DecodedServerFrame{
        .envelope = WireEnvelope<ServerMessage>{
            .request_id = header.request_id,
            .message = std::move(message),
        },
        .bytes_consumed = kWireFrameHeaderSize + header.payload_length,
    };
}

DecodedClientFrameResult decode_client_frame(std::span<const std::byte> bytes) {
    return require_exact_frame(decode_client_frame_prefix(bytes), bytes.size());
}

DecodedServerFrameResult decode_server_frame(std::span<const std::byte> bytes) {
    return require_exact_frame(decode_server_frame_prefix(bytes), bytes.size());
}

ClientStreamParser::ClientStreamParser(std::size_t maximum_buffered_bytes)
    : maximum_buffered_bytes_(maximum_buffered_bytes) {
    buffer_.reserve(std::min(maximum_buffered_bytes_, kWireMaximumPayloadLength + kWireFrameHeaderSize));
}

ClientParseFeedResult ClientStreamParser::feed(std::span<const std::byte> bytes) {
    ClientParseFeedResult result;
    if (failure_.has_value()) {
        result.error = failure_;
        return result;
    }

    process_available(result);
    if (failure_.has_value()) {
        return result;
    }

    std::size_t consumed = 0;
    while (consumed < bytes.size()) {
        compact_if_needed();
        const std::size_t unread = buffer_.size() - read_cursor_;
        if (unread == maximum_buffered_bytes_) {
            fail(result, WireCodecError::BufferLimitExceeded);
            return result;
        }

        const std::size_t append_count = std::min(
            maximum_buffered_bytes_ - unread,
            bytes.size() - consumed);
        buffer_.insert(
            buffer_.end(),
            bytes.begin() + static_cast<std::ptrdiff_t>(consumed),
            bytes.begin() + static_cast<std::ptrdiff_t>(consumed + append_count));
        consumed += append_count;
        process_available(result);
        if (failure_.has_value()) {
            return result;
        }
    }
    compact_if_needed();
    return result;
}

bool ClientStreamParser::failed() const noexcept {
    return failure_.has_value();
}

std::optional<WireCodecError> ClientStreamParser::failure() const noexcept {
    return failure_;
}

bool ClientStreamParser::has_incomplete_frame() const noexcept {
    return !failure_.has_value() && buffer_.size() != read_cursor_;
}

std::size_t ClientStreamParser::buffered_size() const noexcept {
    return buffer_.size() - read_cursor_;
}

void ClientStreamParser::compact_if_needed() {
    if (read_cursor_ == 0) {
        return;
    }
    if (read_cursor_ == buffer_.size()) {
        buffer_.clear();
        read_cursor_ = 0;
        return;
    }
    if (read_cursor_ >= buffer_.size() / 2U || buffer_.size() == maximum_buffered_bytes_) {
        buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(read_cursor_));
        read_cursor_ = 0;
    }
}

void ClientStreamParser::process_available(ClientParseFeedResult& result) {
    while (!failure_.has_value() && read_cursor_ < buffer_.size()) {
        const std::span<const std::byte> unread{
            buffer_.data() + read_cursor_,
            buffer_.size() - read_cursor_};
        const DecodedClientFrameResult decoded = decode_client_frame_prefix(unread);
        if (const auto* error = std::get_if<WireCodecError>(&decoded)) {
            if (*error == WireCodecError::TruncatedInput) {
                return;
            }
            fail(result, *error);
            return;
        }
        const DecodedClientFrame& frame = std::get<DecodedClientFrame>(decoded);
        result.messages.push_back(frame.envelope);
        read_cursor_ += frame.bytes_consumed;
    }
}

void ClientStreamParser::fail(ClientParseFeedResult& result, WireCodecError error) {
    failure_ = error;
    result.error = error;
}

} // namespace matching
