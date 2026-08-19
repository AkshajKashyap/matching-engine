#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "matching/journal.hpp"

namespace matching {
namespace {

[[nodiscard]] std::vector<std::byte> make_bytes(std::initializer_list<std::uint8_t> values) {
    std::vector<std::byte> bytes;
    bytes.reserve(values.size());
    for (const std::uint8_t value : values) {
        bytes.push_back(static_cast<std::byte>(value));
    }
    return bytes;
}

[[nodiscard]] std::vector<std::byte> ascii_bytes(std::string_view text) {
    std::vector<std::byte> bytes;
    bytes.reserve(text.size());
    for (const char character : text) {
        bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(character)));
    }
    return bytes;
}

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

[[nodiscard]] std::vector<std::byte> encoded_record_or_failure(
    JournalSequence sequence,
    const JournalCommand& command) {
    const EncodedJournalRecord result = encode_record(sequence, command);
    if (const auto* bytes = std::get_if<std::vector<std::byte>>(&result)) {
        return *bytes;
    }

    ADD_FAILURE() << "encoding unexpectedly failed";
    return {};
}

void expect_record_error(
    const std::vector<std::byte>& bytes,
    JournalCodecError expected_error) {
    const DecodedJournalRecordResult result = decode_record(bytes);
    ASSERT_TRUE(std::holds_alternative<JournalCodecError>(result));
    EXPECT_EQ(std::get<JournalCodecError>(result), expected_error);
}

void expect_header_error(
    const std::vector<std::byte>& bytes,
    JournalCodecError expected_error) {
    const DecodedJournalFileHeader result = decode_file_header(bytes);
    ASSERT_TRUE(std::holds_alternative<JournalCodecError>(result));
    EXPECT_EQ(std::get<JournalCodecError>(result), expected_error);
}

TEST(JournalCodecTest, Crc32cMatchesIndependentKnownVector) {
    const std::vector<std::byte> input = ascii_bytes("123456789");

    EXPECT_EQ(crc32c(input), 0xE3069283U);
}

TEST(JournalCodecTest, EncodesFileHeaderAsExactGoldenBytes) {
    const std::vector<std::byte> expected = make_bytes({
        0x4D, 0x41, 0x54, 0x43, 0x48, 0x4A, 0x4E, 0x4C,
        0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
    });

    EXPECT_EQ(encode_file_header(), expected);
    const DecodedJournalFileHeader decoded = decode_file_header(expected);
    ASSERT_TRUE(std::holds_alternative<JournalFileHeader>(decoded));
    const JournalFileHeader& header = std::get<JournalFileHeader>(decoded);
    EXPECT_EQ(header.journal_format_version, kJournalFormatVersion);
    EXPECT_EQ(header.matching_rules_version, kMatchingRulesVersion);
}

TEST(JournalCodecTest, StrictlyValidatesFileHeader) {
    const std::vector<std::byte> valid = encode_file_header();

    for (std::size_t length = 0; length < valid.size(); ++length) {
        std::vector<std::byte> truncated(valid.begin(), valid.begin() + static_cast<std::ptrdiff_t>(length));
        expect_header_error(truncated, JournalCodecError::TruncatedInput);
    }

    std::vector<std::byte> bad_magic = valid;
    bad_magic[0] = std::byte{0};
    expect_header_error(bad_magic, JournalCodecError::InvalidFileMagic);

    std::vector<std::byte> bad_journal_version = valid;
    bad_journal_version[9] = std::byte{2};
    expect_header_error(bad_journal_version, JournalCodecError::UnsupportedJournalVersion);

    std::vector<std::byte> bad_matching_rules_version = valid;
    bad_matching_rules_version[11] = std::byte{2};
    expect_header_error(
        bad_matching_rules_version,
        JournalCodecError::UnsupportedMatchingRulesVersion);

    std::vector<std::byte> nonzero_reserved = valid;
    nonzero_reserved[15] = std::byte{1};
    expect_header_error(nonzero_reserved, JournalCodecError::InvalidReservedBytes);

    std::vector<std::byte> trailing = valid;
    trailing.push_back(std::byte{0});
    expect_header_error(trailing, JournalCodecError::UnexpectedTrailingBytes);
}

TEST(JournalCodecTest, EncodesSubmitRecordAsExactGoldenBytes) {
    const JournalCommand command = SubmitLimitOrderCommand{
        .order = order(0x1122334455667788ULL, Side::Buy, 0x0102030405060708ULL, 0xFFEEDDCCBBAA9988ULL),
    };
    const std::vector<std::byte> expected = make_bytes({
        0x4D, 0x4A, 0x52, 0x31, 0x01, 0x01, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x19, 0x01, 0x02, 0x03, 0x04,
        0x05, 0x06, 0x07, 0x08, 0x1B, 0xBB, 0x5C, 0xE0,
        0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0xFF, 0xEE, 0xDD, 0xCC, 0xBB, 0xAA, 0x99,
        0x88,
    });

    EXPECT_EQ(encoded_record_or_failure(JournalSequence{0x0102030405060708ULL}, command), expected);
}

TEST(JournalCodecTest, EncodesCancelRecordAsExactGoldenBytes) {
    const JournalCommand command = CancelOrderCommand{
        .order_id = OrderId{0xFFEEDDCCBBAA9988ULL},
    };
    const std::vector<std::byte> expected = make_bytes({
        0x4D, 0x4A, 0x52, 0x31, 0x01, 0x02, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x08, 0x01, 0x02, 0x03, 0x04,
        0x05, 0x06, 0x07, 0x08, 0xCB, 0x18, 0x66, 0x2F,
        0xFF, 0xEE, 0xDD, 0xCC, 0xBB, 0xAA, 0x99, 0x88,
    });

    EXPECT_EQ(encoded_record_or_failure(JournalSequence{0x0102030405060708ULL}, command), expected);
}

TEST(JournalCodecTest, RoundTripsBuyAndSellSubmitCommands) {
    const std::array<NewLimitOrder, 2> orders{
        order(11, Side::Buy, 101, 7),
        order(12, Side::Sell, 102, 8),
    };

    for (const NewLimitOrder& submitted_order : orders) {
        const JournalCommand command = SubmitLimitOrderCommand{.order = submitted_order};
        const DecodedJournalRecordResult decoded = decode_record(
            encoded_record_or_failure(JournalSequence{42}, command));

        ASSERT_TRUE(std::holds_alternative<DecodedJournalRecord>(decoded));
        const DecodedJournalRecord& record = std::get<DecodedJournalRecord>(decoded);
        EXPECT_EQ(record.record.sequence, JournalSequence{42});
        ASSERT_TRUE(std::holds_alternative<SubmitLimitOrderCommand>(record.record.command));
        const NewLimitOrder& round_tripped =
            std::get<SubmitLimitOrderCommand>(record.record.command).order;
        EXPECT_EQ(round_tripped.id, submitted_order.id);
        EXPECT_EQ(round_tripped.side, submitted_order.side);
        EXPECT_EQ(round_tripped.limit_price, submitted_order.limit_price);
        EXPECT_EQ(round_tripped.quantity, submitted_order.quantity);
    }
}

TEST(JournalCodecTest, RoundTripsRawSideByteWithoutNormalization) {
    constexpr std::uint8_t kRawSide = 0xA5;
    const JournalCommand command = SubmitLimitOrderCommand{
        .order = order(11, static_cast<Side>(kRawSide), 101, 7),
    };

    const std::vector<std::byte> encoded = encoded_record_or_failure(JournalSequence{42}, command);
    ASSERT_EQ(encoded[32], static_cast<std::byte>(kRawSide));
    const DecodedJournalRecordResult decoded = decode_record(encoded);

    ASSERT_TRUE(std::holds_alternative<DecodedJournalRecord>(decoded));
    const auto& decoded_command = std::get<SubmitLimitOrderCommand>(
        std::get<DecodedJournalRecord>(decoded).record.command);
    EXPECT_EQ(static_cast<std::uint8_t>(decoded_command.order.side), kRawSide);
}

TEST(JournalCodecTest, RoundTripsCancelAndMaximumValidValues) {
    const JournalCommand cancel = CancelOrderCommand{
        .order_id = OrderId{std::numeric_limits<std::uint64_t>::max()},
    };
    const DecodedJournalRecordResult decoded_cancel = decode_record(
        encoded_record_or_failure(JournalSequence{1}, cancel));
    ASSERT_TRUE(std::holds_alternative<DecodedJournalRecord>(decoded_cancel));
    const auto& decoded_cancel_command = std::get<CancelOrderCommand>(
        std::get<DecodedJournalRecord>(decoded_cancel).record.command);
    EXPECT_EQ(decoded_cancel_command.order_id.value, std::numeric_limits<std::uint64_t>::max());

    const JournalCommand submit = SubmitLimitOrderCommand{
        .order = order(
            std::numeric_limits<std::uint64_t>::max(),
            Side::Sell,
            std::numeric_limits<std::uint64_t>::max(),
            std::numeric_limits<std::uint64_t>::max()),
    };
    const DecodedJournalRecordResult decoded_submit = decode_record(
        encoded_record_or_failure(JournalSequence{std::numeric_limits<std::uint64_t>::max()}, submit));
    ASSERT_TRUE(std::holds_alternative<DecodedJournalRecord>(decoded_submit));
    const auto& decoded_submit_order = std::get<SubmitLimitOrderCommand>(
        std::get<DecodedJournalRecord>(decoded_submit).record.command).order;
    EXPECT_EQ(decoded_submit_order.id.value, std::numeric_limits<std::uint64_t>::max());
    EXPECT_EQ(decoded_submit_order.limit_price.ticks, std::numeric_limits<std::uint64_t>::max());
    EXPECT_EQ(decoded_submit_order.quantity.units, std::numeric_limits<std::uint64_t>::max());
}

TEST(JournalCodecTest, RejectsReservedZeroJournalSequence) {
    const JournalCommand command = CancelOrderCommand{.order_id = OrderId{1}};
    const EncodedJournalRecord encoded = encode_record(JournalSequence{0}, command);
    ASSERT_TRUE(std::holds_alternative<JournalCodecError>(encoded));
    EXPECT_EQ(std::get<JournalCodecError>(encoded), JournalCodecError::InvalidJournalSequence);

    std::vector<std::byte> valid = encoded_record_or_failure(JournalSequence{1}, command);
    for (std::size_t index = 12; index < 20; ++index) {
        valid[index] = std::byte{0};
    }
    expect_record_error(valid, JournalCodecError::InvalidJournalSequence);
}

TEST(JournalCodecTest, StrictlyRejectsCorruptedRecordFields) {
    const JournalCommand command = SubmitLimitOrderCommand{.order = order(7, Side::Buy, 100, 3)};
    const std::vector<std::byte> valid = encoded_record_or_failure(JournalSequence{1}, command);

    std::vector<std::byte> bad_magic = valid;
    bad_magic[0] = std::byte{0};
    expect_record_error(bad_magic, JournalCodecError::InvalidFrameMagic);

    std::vector<std::byte> bad_record_version = valid;
    bad_record_version[4] = std::byte{2};
    expect_record_error(bad_record_version, JournalCodecError::UnsupportedRecordVersion);

    std::vector<std::byte> bad_record_type = valid;
    bad_record_type[5] = std::byte{3};
    expect_record_error(bad_record_type, JournalCodecError::UnsupportedRecordType);

    std::vector<std::byte> bad_flags = valid;
    bad_flags[7] = std::byte{1};
    expect_record_error(bad_flags, JournalCodecError::InvalidFlags);

    std::vector<std::byte> bad_payload_length = valid;
    bad_payload_length[11] = std::byte{24};
    expect_record_error(bad_payload_length, JournalCodecError::InvalidPayloadLength);

    std::vector<std::byte> oversized_payload_length = valid;
    oversized_payload_length[11] = std::byte{65};
    expect_record_error(oversized_payload_length, JournalCodecError::InvalidPayloadLength);

    std::vector<std::byte> bad_payload = valid;
    bad_payload[24] ^= std::byte{1};
    expect_record_error(bad_payload, JournalCodecError::ChecksumMismatch);

    std::vector<std::byte> bad_checksum = valid;
    bad_checksum[20] ^= std::byte{1};
    expect_record_error(bad_checksum, JournalCodecError::ChecksumMismatch);
}

TEST(JournalCodecTest, DistinguishesTruncatedFramesAndParserBoundaries) {
    const JournalCommand command = SubmitLimitOrderCommand{.order = order(7, Side::Buy, 100, 3)};
    const std::vector<std::byte> valid = encoded_record_or_failure(JournalSequence{1}, command);

    for (std::size_t length = 0; length < valid.size(); ++length) {
        std::vector<std::byte> truncated(valid.begin(), valid.begin() + static_cast<std::ptrdiff_t>(length));
        expect_record_error(truncated, JournalCodecError::TruncatedInput);
    }

    for (std::size_t length = 20; length < 24; ++length) {
        std::vector<std::byte> missing_checksum(
            valid.begin(), valid.begin() + static_cast<std::ptrdiff_t>(length));
        expect_record_error(missing_checksum, JournalCodecError::TruncatedInput);
    }

    std::vector<std::byte> two_frames = valid;
    two_frames.insert(two_frames.end(), valid.begin(), valid.end());
    const DecodedJournalRecordResult prefixed = decode_record_prefix(two_frames);
    ASSERT_TRUE(std::holds_alternative<DecodedJournalRecord>(prefixed));
    EXPECT_EQ(std::get<DecodedJournalRecord>(prefixed).bytes_consumed, valid.size());
    expect_record_error(two_frames, JournalCodecError::UnexpectedTrailingBytes);
}

} // namespace
} // namespace matching
