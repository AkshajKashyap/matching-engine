#include "matching/wire_protocol.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

namespace matching {
namespace {

[[nodiscard]] std::vector<std::byte> bytes(std::initializer_list<std::uint8_t> values) {
    std::vector<std::byte> result;
    result.reserve(values.size());
    for (const std::uint8_t value : values) {
        result.push_back(static_cast<std::byte>(value));
    }
    return result;
}

[[nodiscard]] WireEnvelope<ClientMessage> submit_request(
    RequestId request_id = RequestId{0x0102030405060708ULL}) {
    return WireEnvelope<ClientMessage>{
        .request_id = request_id,
        .message = SubmitLimitOrderRequest{
            .order_id = OrderId{0x1112131415161718ULL},
            .raw_side = 0xFFU,
            .limit_price = Price{0x2122232425262728ULL},
            .quantity = Quantity{0x3132333435363738ULL},
        },
    };
}

[[nodiscard]] WireEnvelope<ClientMessage> cancel_request(
    RequestId request_id = RequestId{0x0102030405060708ULL}) {
    return WireEnvelope<ClientMessage>{
        .request_id = request_id,
        .message = CancelOrderRequest{.order_id = OrderId{0x1112131415161718ULL}},
    };
}

[[nodiscard]] std::vector<std::byte> encoded_client(const WireEnvelope<ClientMessage>& envelope) {
    const EncodedWireFrame encoded = encode_client_frame(envelope);
    EXPECT_TRUE(std::holds_alternative<std::vector<std::byte>>(encoded));
    return std::get<std::vector<std::byte>>(encoded);
}

[[nodiscard]] std::vector<std::byte> encoded_server(const WireEnvelope<ServerMessage>& envelope) {
    const EncodedWireFrame encoded = encode_server_frame(envelope);
    EXPECT_TRUE(std::holds_alternative<std::vector<std::byte>>(encoded));
    return std::get<std::vector<std::byte>>(encoded);
}

void expect_submit(
    const WireEnvelope<ClientMessage>& envelope,
    RequestId request_id,
    OrderId order_id,
    std::uint8_t raw_side,
    Price price,
    Quantity quantity) {
    ASSERT_EQ(envelope.request_id, request_id);
    ASSERT_TRUE(std::holds_alternative<SubmitLimitOrderRequest>(envelope.message));
    const SubmitLimitOrderRequest& request = std::get<SubmitLimitOrderRequest>(envelope.message);
    EXPECT_EQ(request.order_id, order_id);
    EXPECT_EQ(request.raw_side, raw_side);
    EXPECT_EQ(request.limit_price, price);
    EXPECT_EQ(request.quantity, quantity);
}

void expect_cancel(
    const WireEnvelope<ClientMessage>& envelope,
    RequestId request_id,
    OrderId order_id) {
    ASSERT_EQ(envelope.request_id, request_id);
    ASSERT_TRUE(std::holds_alternative<CancelOrderRequest>(envelope.message));
    EXPECT_EQ(std::get<CancelOrderRequest>(envelope.message).order_id, order_id);
}

template <typename Result>
void expect_codec_error(const Result& result, WireCodecError expected) {
    ASSERT_TRUE(std::holds_alternative<WireCodecError>(result));
    EXPECT_EQ(std::get<WireCodecError>(result), expected);
}

TEST(WireProtocolTest, DefinesStableHeaderConstantsAndMessageValues) {
    EXPECT_EQ(kWireProtocolMagic, 0x4D455847U);
    EXPECT_EQ(kWireProtocolVersion, 1U);
    EXPECT_EQ(kWireFrameHeaderSize, 20U);
    EXPECT_EQ(kWireMaximumPayloadLength, 4096U);
    EXPECT_EQ(static_cast<std::uint16_t>(WireMessageType::SubmitLimitOrder), 1U);
    EXPECT_EQ(static_cast<std::uint16_t>(WireMessageType::CancelOrder), 2U);
    EXPECT_EQ(static_cast<std::uint16_t>(WireMessageType::SubmitResult), 101U);
    EXPECT_EQ(static_cast<std::uint16_t>(WireMessageType::CancelResult), 102U);
    EXPECT_EQ(static_cast<std::uint16_t>(WireMessageType::ProtocolError), 103U);
    EXPECT_EQ(static_cast<std::uint16_t>(WireMessageType::EngineUnavailable), 104U);
}

TEST(WireProtocolGoldenBytesTest, EncodesSubmitLimitOrderRequestExactly) {
    const std::vector<std::byte> actual = encoded_client(submit_request());
    const std::vector<std::byte> expected = bytes({
        0x4D, 0x45, 0x58, 0x47, 0x00, 0x01, 0x00, 0x01,
        0x00, 0x00, 0x00, 0x19, 0x01, 0x02, 0x03, 0x04,
        0x05, 0x06, 0x07, 0x08, 0x11, 0x12, 0x13, 0x14,
        0x15, 0x16, 0x17, 0x18, 0xFF, 0x21, 0x22, 0x23,
        0x24, 0x25, 0x26, 0x27, 0x28, 0x31, 0x32, 0x33,
        0x34, 0x35, 0x36, 0x37, 0x38,
    });
    EXPECT_EQ(actual, expected);
}

TEST(WireProtocolGoldenBytesTest, EncodesCancelOrderRequestExactly) {
    const std::vector<std::byte> actual = encoded_client(cancel_request(RequestId{0}));
    const std::vector<std::byte> expected = bytes({
        0x4D, 0x45, 0x58, 0x47, 0x00, 0x01, 0x00, 0x02,
        0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x11, 0x12, 0x13, 0x14,
        0x15, 0x16, 0x17, 0x18,
    });
    EXPECT_EQ(actual, expected);
}

TEST(WireProtocolGoldenBytesTest, EncodesAcceptedAndRejectedSubmitResultsExactly) {
    const WireEnvelope<ServerMessage> accepted{
        .request_id = RequestId{0xA1A2A3A4A5A6A7A8ULL},
        .message = SubmitResultResponse{
            .status = WireSubmitStatus::Accepted,
            .rejection_code = WireRejectionCode::None,
            .executed_quantity = Quantity{0x0102030405060708ULL},
            .resting_quantity = Quantity{0x1112131415161718ULL},
        },
    };
    EXPECT_EQ(encoded_server(accepted), bytes({
        0x4D, 0x45, 0x58, 0x47, 0x00, 0x01, 0x00, 0x65,
        0x00, 0x00, 0x00, 0x12, 0xA1, 0xA2, 0xA3, 0xA4,
        0xA5, 0xA6, 0xA7, 0xA8, 0x01, 0x00, 0x01, 0x02,
        0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x11, 0x12,
        0x13, 0x14, 0x15, 0x16, 0x17, 0x18,
    }));

    const WireEnvelope<ServerMessage> rejected{
        .request_id = RequestId{0x0102030405060708ULL},
        .message = SubmitResultResponse{
            .status = WireSubmitStatus::Rejected,
            .rejection_code = WireRejectionCode::DuplicateOrderId,
            .executed_quantity = Quantity{0},
            .resting_quantity = Quantity{0},
        },
    };
    EXPECT_EQ(encoded_server(rejected), bytes({
        0x4D, 0x45, 0x58, 0x47, 0x00, 0x01, 0x00, 0x65,
        0x00, 0x00, 0x00, 0x12, 0x01, 0x02, 0x03, 0x04,
        0x05, 0x06, 0x07, 0x08, 0x02, 0x05, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    }));
}

TEST(WireProtocolGoldenBytesTest, EncodesCancelledAndNotFoundCancelResultsExactly) {
    const WireEnvelope<ServerMessage> cancelled{
        .request_id = RequestId{0x0102030405060708ULL},
        .message = CancelResultResponse{
            .status = WireCancelStatus::Cancelled,
            .cancelled_quantity = Quantity{0x1112131415161718ULL},
        },
    };
    EXPECT_EQ(encoded_server(cancelled), bytes({
        0x4D, 0x45, 0x58, 0x47, 0x00, 0x01, 0x00, 0x66,
        0x00, 0x00, 0x00, 0x09, 0x01, 0x02, 0x03, 0x04,
        0x05, 0x06, 0x07, 0x08, 0x01, 0x11, 0x12, 0x13,
        0x14, 0x15, 0x16, 0x17, 0x18,
    }));

    const WireEnvelope<ServerMessage> not_found{
        .request_id = RequestId{0x0102030405060708ULL},
        .message = CancelResultResponse{
            .status = WireCancelStatus::NotFound,
            .cancelled_quantity = Quantity{0},
        },
    };
    EXPECT_EQ(encoded_server(not_found), bytes({
        0x4D, 0x45, 0x58, 0x47, 0x00, 0x01, 0x00, 0x66,
        0x00, 0x00, 0x00, 0x09, 0x01, 0x02, 0x03, 0x04,
        0x05, 0x06, 0x07, 0x08, 0x02, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00,
    }));
}

TEST(WireProtocolGoldenBytesTest, EncodesProtocolErrorAndEngineUnavailableExactly) {
    const WireEnvelope<ServerMessage> protocol_error{
        .request_id = RequestId{0x0102030405060708ULL},
        .message = ProtocolErrorResponse{.code = WireProtocolErrorCode::InvalidPayloadLength},
    };
    EXPECT_EQ(encoded_server(protocol_error), bytes({
        0x4D, 0x45, 0x58, 0x47, 0x00, 0x01, 0x00, 0x67,
        0x00, 0x00, 0x00, 0x02, 0x01, 0x02, 0x03, 0x04,
        0x05, 0x06, 0x07, 0x08, 0x00, 0x05,
    }));

    const WireEnvelope<ServerMessage> unavailable{
        .request_id = RequestId{0x0102030405060708ULL},
        .message = EngineUnavailableResponse{},
    };
    EXPECT_EQ(encoded_server(unavailable), bytes({
        0x4D, 0x45, 0x58, 0x47, 0x00, 0x01, 0x00, 0x68,
        0x00, 0x00, 0x00, 0x01, 0x01, 0x02, 0x03, 0x04,
        0x05, 0x06, 0x07, 0x08, 0x01,
    }));
}

TEST(WireProtocolTest, RoundTripsClientRequestsIncludingRawSideAndBoundaryIdentifiers) {
    const std::array<WireEnvelope<ClientMessage>, 3> messages{
        submit_request(RequestId{0}),
        WireEnvelope<ClientMessage>{
            .request_id = RequestId{std::numeric_limits<std::uint64_t>::max()},
            .message = SubmitLimitOrderRequest{
                .order_id = OrderId{std::numeric_limits<std::uint64_t>::max()},
                .raw_side = static_cast<std::uint8_t>(Side::Buy),
                .limit_price = Price{std::numeric_limits<std::uint64_t>::max()},
                .quantity = Quantity{std::numeric_limits<std::uint64_t>::max()},
            },
        },
        cancel_request(RequestId{std::numeric_limits<std::uint64_t>::max()}),
    };

    for (const WireEnvelope<ClientMessage>& message : messages) {
        const DecodedClientFrameResult decoded = decode_client_frame(encoded_client(message));
        ASSERT_TRUE(std::holds_alternative<DecodedClientFrame>(decoded));
        const DecodedClientFrame& frame = std::get<DecodedClientFrame>(decoded);
        EXPECT_EQ(frame.bytes_consumed, encoded_client(message).size());
        EXPECT_EQ(frame.envelope.request_id, message.request_id);
        EXPECT_EQ(frame.envelope.message.index(), message.message.index());
        if (const auto* submit = std::get_if<SubmitLimitOrderRequest>(&message.message)) {
            expect_submit(
                frame.envelope,
                message.request_id,
                submit->order_id,
                submit->raw_side,
                submit->limit_price,
                submit->quantity);
        } else {
            expect_cancel(
                frame.envelope,
                message.request_id,
                std::get<CancelOrderRequest>(message.message).order_id);
        }
    }
}

TEST(WireProtocolTest, RoundTripsEveryServerResponseType) {
    const std::array<WireEnvelope<ServerMessage>, 5> messages{
        WireEnvelope<ServerMessage>{
            .request_id = RequestId{1},
            .message = SubmitResultResponse{
                .status = WireSubmitStatus::Accepted,
                .rejection_code = WireRejectionCode::None,
                .executed_quantity = Quantity{2},
                .resting_quantity = Quantity{3},
            },
        },
        WireEnvelope<ServerMessage>{
            .request_id = RequestId{2},
            .message = SubmitResultResponse{
                .status = WireSubmitStatus::Rejected,
                .rejection_code = WireRejectionCode::InvalidPrice,
                .executed_quantity = Quantity{0},
                .resting_quantity = Quantity{0},
            },
        },
        WireEnvelope<ServerMessage>{
            .request_id = RequestId{3},
            .message = CancelResultResponse{
                .status = WireCancelStatus::Cancelled,
                .cancelled_quantity = Quantity{4},
            },
        },
        WireEnvelope<ServerMessage>{
            .request_id = RequestId{4},
            .message = ProtocolErrorResponse{.code = WireProtocolErrorCode::BadMagic},
        },
        WireEnvelope<ServerMessage>{
            .request_id = RequestId{5},
            .message = EngineUnavailableResponse{},
        },
    };

    for (const WireEnvelope<ServerMessage>& message : messages) {
        const std::vector<std::byte> encoded = encoded_server(message);
        const DecodedServerFrameResult decoded = decode_server_frame(encoded);
        ASSERT_TRUE(std::holds_alternative<DecodedServerFrame>(decoded));
        EXPECT_EQ(std::get<DecodedServerFrame>(decoded).envelope.request_id, message.request_id);
        EXPECT_EQ(
            std::get<DecodedServerFrame>(decoded).envelope.message.index(),
            message.message.index());
        EXPECT_EQ(std::get<DecodedServerFrame>(decoded).bytes_consumed, encoded.size());
    }
}

TEST(WireProtocolMappingTest, MapsEveryDomainRejectionAndCancelStatusExplicitly) {
    const std::array<std::pair<RejectionReason, WireRejectionCode>, 8> rejections{{
        {RejectionReason::InvalidOrderId, WireRejectionCode::InvalidOrderId},
        {RejectionReason::InvalidSide, WireRejectionCode::InvalidSide},
        {RejectionReason::InvalidPrice, WireRejectionCode::InvalidPrice},
        {RejectionReason::InvalidQuantity, WireRejectionCode::InvalidQuantity},
        {RejectionReason::DuplicateOrderId, WireRejectionCode::DuplicateOrderId},
        {RejectionReason::QuantityOverflow, WireRejectionCode::QuantityOverflow},
        {RejectionReason::SequenceExhausted, WireRejectionCode::SequenceExhausted},
        {RejectionReason::TradeIdExhausted, WireRejectionCode::TradeIdExhausted},
    }};
    for (const auto& [domain, wire] : rejections) {
        EXPECT_EQ(to_wire_rejection_code(domain), wire);
        EXPECT_EQ(to_domain_rejection_reason(wire), domain);
    }
    EXPECT_FALSE(to_domain_rejection_reason(WireRejectionCode::None).has_value());

    const std::array<std::pair<CancelStatus, WireCancelStatus>, 2> cancellations{{
        {CancelStatus::Cancelled, WireCancelStatus::Cancelled},
        {CancelStatus::NotFound, WireCancelStatus::NotFound},
    }};
    for (const auto& [domain, wire] : cancellations) {
        EXPECT_EQ(to_wire_cancel_status(domain), wire);
        EXPECT_EQ(to_domain_cancel_status(wire), domain);
    }
}

TEST(WireProtocolTest, ExactDecoderRejectsTrailingFramesWhilePrefixDecoderPreservesBoundary) {
    std::vector<std::byte> combined = encoded_client(submit_request());
    const std::vector<std::byte> cancel = encoded_client(cancel_request());
    combined.insert(combined.end(), cancel.begin(), cancel.end());

    expect_codec_error(decode_client_frame(combined), WireCodecError::UnexpectedTrailingBytes);
    const DecodedClientFrameResult first = decode_client_frame_prefix(combined);
    ASSERT_TRUE(std::holds_alternative<DecodedClientFrame>(first));
    EXPECT_EQ(std::get<DecodedClientFrame>(first).bytes_consumed, encoded_client(submit_request()).size());
}

TEST(WireProtocolInvalidInputTest, DistinguishesIncompleteHeadersAndPayloads) {
    const std::vector<std::byte> frame = encoded_client(submit_request());
    for (std::size_t size = 0; size < kWireFrameHeaderSize; ++size) {
        const DecodedClientFrameResult decoded = decode_client_frame_prefix(std::span{frame}.first(size));
        ASSERT_TRUE(std::holds_alternative<WireCodecError>(decoded)) << "header size " << size;
        EXPECT_EQ(std::get<WireCodecError>(decoded), WireCodecError::TruncatedInput) << "header size " << size;
    }
    for (std::size_t size = kWireFrameHeaderSize; size < frame.size(); ++size) {
        const DecodedClientFrameResult decoded = decode_client_frame_prefix(std::span{frame}.first(size));
        ASSERT_TRUE(std::holds_alternative<WireCodecError>(decoded)) << "payload size " << size;
        EXPECT_EQ(std::get<WireCodecError>(decoded), WireCodecError::TruncatedInput) << "payload size " << size;
    }
}

TEST(WireProtocolInvalidInputTest, RejectsKnownMalformedHeaderFieldsAndWrongDirection) {
    const std::vector<std::byte> valid = encoded_client(submit_request());
    auto expect_error = [&valid](std::size_t offset, std::uint8_t value, WireCodecError expected) {
        std::vector<std::byte> frame = valid;
        frame[offset] = static_cast<std::byte>(value);
        expect_codec_error(decode_client_frame_prefix(frame), expected);
    };

    expect_error(0, 0x00, WireCodecError::BadMagic);
    expect_error(5, 0x02, WireCodecError::UnsupportedVersion);
    expect_error(7, 0x7F, WireCodecError::UnsupportedMessageType);
    expect_error(11, 0x1A, WireCodecError::InvalidPayloadLength);

    std::vector<std::byte> oversized = valid;
    oversized[8] = static_cast<std::byte>(0x00);
    oversized[9] = static_cast<std::byte>(0x00);
    oversized[10] = static_cast<std::byte>(0x10);
    oversized[11] = static_cast<std::byte>(0x01);
    expect_codec_error(decode_client_frame_prefix(oversized), WireCodecError::PayloadTooLarge);

    const WireEnvelope<ServerMessage> response{
        .request_id = RequestId{1},
        .message = EngineUnavailableResponse{},
    };
    expect_codec_error(
        decode_client_frame_prefix(encoded_server(response)),
        WireCodecError::UnsupportedMessageType);
}

TEST(WireProtocolInvalidInputTest, RejectsMalformedServerPayloadAndInvalidEncoderValues) {
    WireEnvelope<ServerMessage> response{
        .request_id = RequestId{1},
        .message = SubmitResultResponse{
            .status = WireSubmitStatus::Accepted,
            .rejection_code = WireRejectionCode::None,
            .executed_quantity = Quantity{1},
            .resting_quantity = Quantity{2},
        },
    };
    std::vector<std::byte> bytes_response = encoded_server(response);
    bytes_response[kWireFrameHeaderSize + 1U] = static_cast<std::byte>(WireRejectionCode::InvalidPrice);
    expect_codec_error(decode_server_frame(bytes_response), WireCodecError::MalformedPayload);

    response.message = SubmitResultResponse{
        .status = WireSubmitStatus::Accepted,
        .rejection_code = WireRejectionCode::InvalidPrice,
        .executed_quantity = Quantity{0},
        .resting_quantity = Quantity{0},
    };
    expect_codec_error(encode_server_frame(response), WireCodecError::MalformedPayload);
}

TEST(WireProtocolParserTest, HandlesEveryFragmentationPointForSubmitAndCancel) {
    const std::array<WireEnvelope<ClientMessage>, 2> requests{submit_request(), cancel_request()};
    for (const WireEnvelope<ClientMessage>& request : requests) {
        const std::vector<std::byte> frame = encoded_client(request);
        for (std::size_t split = 0; split <= frame.size(); ++split) {
            ClientStreamParser parser;
            ClientParseFeedResult first = parser.feed(std::span{frame}.first(split));
            ASSERT_FALSE(first.error.has_value()) << "split " << split;
            EXPECT_EQ(first.messages.size(), split == frame.size() ? 1U : 0U) << "split " << split;
            if (split < frame.size()) {
                EXPECT_TRUE(parser.has_incomplete_frame() || split == 0U);
                ClientParseFeedResult second = parser.feed(std::span{frame}.subspan(split));
                ASSERT_FALSE(second.error.has_value()) << "split " << split;
                ASSERT_EQ(second.messages.size(), 1U) << "split " << split;
                EXPECT_EQ(second.messages.front().request_id, request.request_id);
            }
        }
    }
}

TEST(WireProtocolParserTest, HandlesOneByteAtATimeAndEmptyInput) {
    const WireEnvelope<ClientMessage> request = submit_request();
    const std::vector<std::byte> frame = encoded_client(request);
    ClientStreamParser parser;
    EXPECT_TRUE(parser.feed({}).messages.empty());
    for (std::size_t index = 0; index < frame.size(); ++index) {
        const ClientParseFeedResult result = parser.feed(std::span{frame}.subspan(index, 1));
        ASSERT_FALSE(result.error.has_value());
        EXPECT_EQ(result.messages.size(), index + 1U == frame.size() ? 1U : 0U);
    }
    EXPECT_FALSE(parser.has_incomplete_frame());
}

TEST(WireProtocolParserTest, EmitsConcatenatedFramesAndRetainsPartialFollowingFrame) {
    const std::vector<std::byte> submit = encoded_client(submit_request(RequestId{10}));
    const std::vector<std::byte> cancel = encoded_client(cancel_request(RequestId{11}));
    const std::vector<std::byte> second_submit = encoded_client(submit_request(RequestId{12}));
    std::vector<std::byte> combined = submit;
    combined.insert(combined.end(), cancel.begin(), cancel.end());
    combined.insert(combined.end(), second_submit.begin(), second_submit.begin() + 7);

    ClientStreamParser parser;
    ClientParseFeedResult first = parser.feed(combined);
    ASSERT_FALSE(first.error.has_value());
    ASSERT_EQ(first.messages.size(), 2U);
    expect_submit(first.messages[0], RequestId{10}, OrderId{0x1112131415161718ULL}, 0xFFU,
                  Price{0x2122232425262728ULL}, Quantity{0x3132333435363738ULL});
    expect_cancel(first.messages[1], RequestId{11}, OrderId{0x1112131415161718ULL});
    EXPECT_TRUE(parser.has_incomplete_frame());

    ClientParseFeedResult second = parser.feed(std::span{second_submit}.subspan(7));
    ASSERT_FALSE(second.error.has_value());
    ASSERT_EQ(second.messages.size(), 1U);
    EXPECT_EQ(second.messages[0].request_id, RequestId{12});
    EXPECT_FALSE(parser.has_incomplete_frame());
}

TEST(WireProtocolParserTest, PreservesCancelThenSubmitAndMultipleSubmitWireOrder) {
    const std::vector<std::byte> cancel = encoded_client(cancel_request(RequestId{21}));
    const std::vector<std::byte> first_submit = encoded_client(submit_request(RequestId{22}));
    const std::vector<std::byte> second_submit = encoded_client(submit_request(RequestId{23}));
    std::vector<std::byte> combined = cancel;
    combined.insert(combined.end(), first_submit.begin(), first_submit.end());
    combined.insert(combined.end(), second_submit.begin(), second_submit.end());

    ClientStreamParser parser;
    const ClientParseFeedResult result = parser.feed(combined);
    ASSERT_FALSE(result.error.has_value());
    ASSERT_EQ(result.messages.size(), 3U);
    expect_cancel(result.messages[0], RequestId{21}, OrderId{0x1112131415161718ULL});
    EXPECT_EQ(result.messages[1].request_id, RequestId{22});
    EXPECT_EQ(result.messages[2].request_id, RequestId{23});
    ASSERT_TRUE(std::holds_alternative<SubmitLimitOrderRequest>(result.messages[1].message));
    ASSERT_TRUE(std::holds_alternative<SubmitLimitOrderRequest>(result.messages[2].message));
}

TEST(WireProtocolParserTest, IsTerminalAfterMalformedInputAndDoesNotResynchronize) {
    std::vector<std::byte> malformed = encoded_client(submit_request());
    malformed[0] = static_cast<std::byte>(0x00);
    const std::vector<std::byte> valid = encoded_client(cancel_request());
    malformed.insert(malformed.end(), valid.begin(), valid.end());

    ClientStreamParser parser;
    const ClientParseFeedResult first = parser.feed(malformed);
    EXPECT_TRUE(first.messages.empty());
    EXPECT_EQ(first.error, WireCodecError::BadMagic);
    EXPECT_TRUE(parser.failed());
    EXPECT_EQ(parser.failure(), WireCodecError::BadMagic);

    const ClientParseFeedResult second = parser.feed(valid);
    EXPECT_TRUE(second.messages.empty());
    EXPECT_EQ(second.error, WireCodecError::BadMagic);
}

TEST(WireProtocolParserTest, EnforcesConfiguredBufferLimitAndExposesEofState) {
    const std::vector<std::byte> frame = encoded_client(submit_request());
    ClientStreamParser parser{20};
    const ClientParseFeedResult first = parser.feed(std::span{frame}.first(20));
    EXPECT_FALSE(first.error.has_value());
    EXPECT_TRUE(parser.has_incomplete_frame());
    EXPECT_EQ(parser.buffered_size(), 20U);

    const ClientParseFeedResult second = parser.feed(std::span{frame}.subspan(20, 1));
    EXPECT_EQ(second.error, WireCodecError::BufferLimitExceeded);
    EXPECT_TRUE(parser.failed());
    EXPECT_FALSE(parser.has_incomplete_frame());
}

} // namespace
} // namespace matching
