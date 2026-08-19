#include "matching/journal.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <type_traits>
#include <utility>

namespace matching {
namespace {

static_assert(std::is_same_v<std::underlying_type_t<Side>, std::uint8_t>);

constexpr std::size_t kFormatVersionOffset = 8;
constexpr std::size_t kMatchingRulesVersionOffset = 10;
constexpr std::size_t kReservedOffset = 12;

constexpr std::size_t kRecordVersionOffset = 4;
constexpr std::size_t kRecordTypeOffset = 5;
constexpr std::size_t kFlagsOffset = 6;
constexpr std::size_t kPayloadLengthOffset = 8;
constexpr std::size_t kSequenceOffset = 12;
constexpr std::size_t kChecksumOffset = 20;
constexpr std::size_t kPayloadOffset = 24;
constexpr std::size_t kCrcHeaderLength = 16;

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
    for (std::uint32_t shift = 24; shift > 0; shift -= 8) {
        append_u8(bytes, static_cast<std::uint8_t>(value >> shift));
    }
    append_u8(bytes, static_cast<std::uint8_t>(value));
}

void append_u64(std::vector<std::byte>& bytes, std::uint64_t value) {
    for (std::uint32_t shift = 56; shift > 0; shift -= 8) {
        append_u8(bytes, static_cast<std::uint8_t>(value >> shift));
    }
    append_u8(bytes, static_cast<std::uint8_t>(value));
}

[[nodiscard]] bool has_bytes(std::span<const std::byte> bytes, std::size_t offset, std::size_t count) {
    return offset <= bytes.size() && count <= bytes.size() - offset;
}

[[nodiscard]] std::uint16_t read_u16(std::span<const std::byte> bytes, std::size_t offset) noexcept {
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(byte_value(bytes[offset])) << 8U) |
        static_cast<std::uint16_t>(byte_value(bytes[offset + 1U])));
}

[[nodiscard]] std::uint32_t read_u32(std::span<const std::byte> bytes, std::size_t offset) noexcept {
    std::uint32_t value = 0;
    for (std::size_t index = 0; index < 4; ++index) {
        value = static_cast<std::uint32_t>(
            (value << 8U) | static_cast<std::uint32_t>(byte_value(bytes[offset + index])));
    }
    return value;
}

[[nodiscard]] std::uint64_t read_u64(std::span<const std::byte> bytes, std::size_t offset) noexcept {
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < 8; ++index) {
        value = (value << 8U) | static_cast<std::uint64_t>(byte_value(bytes[offset + index]));
    }
    return value;
}

[[nodiscard]] bool starts_with(
    std::span<const std::byte> bytes,
    const auto& magic) noexcept {
    return bytes.size() >= magic.size() &&
           std::equal(magic.begin(), magic.end(), bytes.begin());
}

[[nodiscard]] std::uint32_t crc32c_update(
    std::uint32_t state,
    std::span<const std::byte> bytes) noexcept {
    constexpr std::uint32_t kReflectedPolynomial = 0x82F63B78U;
    std::uint32_t crc = state;
    for (const std::byte byte : bytes) {
        crc ^= static_cast<std::uint32_t>(byte_value(byte));
        for (std::uint32_t bit = 0; bit < 8; ++bit) {
            const std::uint32_t mask = 0U - (crc & 1U);
            crc = (crc >> 1U) ^ (kReflectedPolynomial & mask);
        }
    }
    return crc;
}

[[nodiscard]] std::uint32_t frame_crc(
    std::span<const std::byte> header_and_payload,
    std::size_t payload_length) noexcept {
    std::uint32_t crc = crc32c_update(
        std::numeric_limits<std::uint32_t>::max(),
        header_and_payload.subspan(kRecordVersionOffset, kCrcHeaderLength));
    crc = crc32c_update(
        crc,
        header_and_payload.subspan(kPayloadOffset, payload_length));
    return ~crc;
}

[[nodiscard]] std::size_t expected_payload_length(JournalRecordType type) noexcept {
    switch (type) {
    case JournalRecordType::SubmitLimitOrder:
        return kSubmitLimitOrderPayloadSize;
    case JournalRecordType::CancelOrder:
        return kCancelOrderPayloadSize;
    }
    return 0;
}

[[nodiscard]] std::variant<JournalRecordType, JournalCodecError> decode_record_type(
    std::uint8_t raw_type) {
    switch (raw_type) {
    case static_cast<std::uint8_t>(JournalRecordType::SubmitLimitOrder):
        return JournalRecordType::SubmitLimitOrder;
    case static_cast<std::uint8_t>(JournalRecordType::CancelOrder):
        return JournalRecordType::CancelOrder;
    default:
        return JournalCodecError::UnsupportedRecordType;
    }
}

[[nodiscard]] std::vector<std::byte> encode_payload(const JournalCommand& command) {
    return std::visit(
        [](const auto& typed_command) {
            using Command = std::decay_t<decltype(typed_command)>;
            std::vector<std::byte> payload;
            if constexpr (std::is_same_v<Command, SubmitLimitOrderCommand>) {
                payload.reserve(kSubmitLimitOrderPayloadSize);
                append_u64(payload, typed_command.order.id.value);
                append_u8(
                    payload,
                    static_cast<std::underlying_type_t<Side>>(typed_command.order.side));
                append_u64(payload, typed_command.order.limit_price.ticks);
                append_u64(payload, typed_command.order.quantity.units);
            } else {
                payload.reserve(kCancelOrderPayloadSize);
                append_u64(payload, typed_command.order_id.value);
            }
            return payload;
        },
        command);
}

[[nodiscard]] JournalRecordType record_type_for(const JournalCommand& command) noexcept {
    return std::holds_alternative<SubmitLimitOrderCommand>(command)
               ? JournalRecordType::SubmitLimitOrder
               : JournalRecordType::CancelOrder;
}

} // namespace

std::vector<std::byte> encode_file_header() {
    std::vector<std::byte> bytes;
    bytes.reserve(kJournalFileHeaderSize);
    bytes.insert(bytes.end(), kJournalFileMagic.begin(), kJournalFileMagic.end());
    append_u16(bytes, kJournalFormatVersion);
    append_u16(bytes, kMatchingRulesVersion);
    append_u32(bytes, 0);
    return bytes;
}

DecodedJournalFileHeader decode_file_header(std::span<const std::byte> bytes) {
    if (bytes.size() < kJournalFileHeaderSize) {
        return JournalCodecError::TruncatedInput;
    }
    if (bytes.size() > kJournalFileHeaderSize) {
        return JournalCodecError::UnexpectedTrailingBytes;
    }
    if (!starts_with(bytes, kJournalFileMagic)) {
        return JournalCodecError::InvalidFileMagic;
    }
    if (read_u16(bytes, kFormatVersionOffset) != kJournalFormatVersion) {
        return JournalCodecError::UnsupportedJournalVersion;
    }
    if (read_u16(bytes, kMatchingRulesVersionOffset) != kMatchingRulesVersion) {
        return JournalCodecError::UnsupportedMatchingRulesVersion;
    }
    if (read_u32(bytes, kReservedOffset) != 0) {
        return JournalCodecError::InvalidReservedBytes;
    }
    return JournalFileHeader{};
}

EncodedJournalRecord encode_record(JournalSequence sequence, const JournalCommand& command) {
    if (sequence.value == 0) {
        return JournalCodecError::InvalidJournalSequence;
    }

    const JournalRecordType type = record_type_for(command);
    const std::vector<std::byte> payload = encode_payload(command);
    std::vector<std::byte> bytes;
    bytes.reserve(kJournalFrameHeaderSize + payload.size());
    bytes.insert(bytes.end(), kJournalFrameMagic.begin(), kJournalFrameMagic.end());
    append_u8(bytes, kJournalRecordVersion);
    append_u8(bytes, static_cast<std::uint8_t>(type));
    append_u16(bytes, 0);
    append_u32(bytes, static_cast<std::uint32_t>(payload.size()));
    append_u64(bytes, sequence.value);
    append_u32(bytes, 0);
    bytes.insert(bytes.end(), payload.begin(), payload.end());

    const std::uint32_t checksum = frame_crc(bytes, payload.size());
    bytes[kChecksumOffset] = static_cast<std::byte>(checksum >> 24U);
    bytes[kChecksumOffset + 1U] = static_cast<std::byte>(checksum >> 16U);
    bytes[kChecksumOffset + 2U] = static_cast<std::byte>(checksum >> 8U);
    bytes[kChecksumOffset + 3U] = static_cast<std::byte>(checksum);
    return bytes;
}

DecodedJournalRecordResult decode_record_prefix(std::span<const std::byte> bytes) {
    if (bytes.size() < kJournalFrameHeaderSize) {
        return JournalCodecError::TruncatedInput;
    }
    if (!starts_with(bytes, kJournalFrameMagic)) {
        return JournalCodecError::InvalidFrameMagic;
    }
    if (byte_value(bytes[kRecordVersionOffset]) != kJournalRecordVersion) {
        return JournalCodecError::UnsupportedRecordVersion;
    }

    const auto type_result = decode_record_type(byte_value(bytes[kRecordTypeOffset]));
    if (const auto* error = std::get_if<JournalCodecError>(&type_result)) {
        return *error;
    }
    const JournalRecordType type = std::get<JournalRecordType>(type_result);

    if (read_u16(bytes, kFlagsOffset) != 0) {
        return JournalCodecError::InvalidFlags;
    }

    const std::uint32_t raw_payload_length = read_u32(bytes, kPayloadLengthOffset);
    if (raw_payload_length > kJournalMaximumPayloadLength ||
        raw_payload_length != expected_payload_length(type)) {
        return JournalCodecError::InvalidPayloadLength;
    }
    const std::size_t payload_length = static_cast<std::size_t>(raw_payload_length);
    if (!has_bytes(bytes, kPayloadOffset, payload_length)) {
        return JournalCodecError::TruncatedInput;
    }

    const JournalSequence sequence{read_u64(bytes, kSequenceOffset)};
    if (sequence.value == 0) {
        return JournalCodecError::InvalidJournalSequence;
    }

    if (read_u32(bytes, kChecksumOffset) != frame_crc(bytes, payload_length)) {
        return JournalCodecError::ChecksumMismatch;
    }

    JournalCommand command;
    if (type == JournalRecordType::SubmitLimitOrder) {
        command = SubmitLimitOrderCommand{
            .order = NewLimitOrder{
                .id = OrderId{read_u64(bytes, kPayloadOffset)},
                .side = static_cast<Side>(byte_value(bytes[kPayloadOffset + 8U])),
                .limit_price = Price{read_u64(bytes, kPayloadOffset + 9U)},
                .quantity = Quantity{read_u64(bytes, kPayloadOffset + 17U)},
            },
        };
    } else {
        command = CancelOrderCommand{
            .order_id = OrderId{read_u64(bytes, kPayloadOffset)},
        };
    }

    return DecodedJournalRecord{
        .record = JournalRecord{
            .sequence = sequence,
            .command = std::move(command),
        },
        .bytes_consumed = kJournalFrameHeaderSize + payload_length,
    };
}

DecodedJournalRecordResult decode_record(std::span<const std::byte> bytes) {
    DecodedJournalRecordResult result = decode_record_prefix(bytes);
    if (const auto* error = std::get_if<JournalCodecError>(&result)) {
        return *error;
    }
    const DecodedJournalRecord& decoded = std::get<DecodedJournalRecord>(result);
    if (decoded.bytes_consumed != bytes.size()) {
        return JournalCodecError::UnexpectedTrailingBytes;
    }
    return result;
}

std::uint32_t crc32c(std::span<const std::byte> bytes) noexcept {
    return ~crc32c_update(std::numeric_limits<std::uint32_t>::max(), bytes);
}

} // namespace matching
