#pragma once

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <span>
#include <variant>
#include <vector>

#include "matching/order.hpp"

namespace matching {

struct JournalSequence {
    std::uint64_t value{};

    constexpr explicit JournalSequence(std::uint64_t value_in = 0) noexcept : value(value_in) {}

    constexpr auto operator<=>(const JournalSequence&) const = default;
};

struct SubmitLimitOrderCommand {
    NewLimitOrder order;
};

struct CancelOrderCommand {
    OrderId order_id;
};

using JournalCommand = std::variant<SubmitLimitOrderCommand, CancelOrderCommand>;

enum class JournalRecordType : std::uint8_t {
    SubmitLimitOrder = 1,
    CancelOrder = 2,
};

enum class JournalCodecError : std::uint8_t {
    InvalidFileMagic,
    UnsupportedJournalVersion,
    UnsupportedMatchingRulesVersion,
    InvalidReservedBytes,
    InvalidFrameMagic,
    UnsupportedRecordVersion,
    UnsupportedRecordType,
    InvalidFlags,
    InvalidPayloadLength,
    InvalidJournalSequence,
    ChecksumMismatch,
    TruncatedInput,
    UnexpectedTrailingBytes,
};

inline constexpr std::array<std::byte, 8> kJournalFileMagic{
    std::byte{'M'}, std::byte{'A'}, std::byte{'T'}, std::byte{'C'},
    std::byte{'H'}, std::byte{'J'}, std::byte{'N'}, std::byte{'L'},
};
inline constexpr std::uint16_t kJournalFormatVersion = 1;
inline constexpr std::uint16_t kMatchingRulesVersion = 1;
inline constexpr std::array<std::byte, 4> kJournalFrameMagic{
    std::byte{'M'}, std::byte{'J'}, std::byte{'R'}, std::byte{'1'},
};
inline constexpr std::uint8_t kJournalRecordVersion = 1;
inline constexpr std::size_t kJournalFileHeaderSize = 16;
inline constexpr std::size_t kJournalFrameHeaderSize = 24;
inline constexpr std::size_t kJournalMaximumPayloadLength = 64;
inline constexpr std::size_t kSubmitLimitOrderPayloadSize = 25;
inline constexpr std::size_t kCancelOrderPayloadSize = 8;

struct JournalFileHeader {
    std::uint16_t journal_format_version{kJournalFormatVersion};
    std::uint16_t matching_rules_version{kMatchingRulesVersion};
};

struct JournalRecord {
    JournalSequence sequence;
    JournalCommand command;
};

struct DecodedJournalRecord {
    JournalRecord record;
    std::size_t bytes_consumed{};
};

using EncodedJournalRecord = std::variant<std::vector<std::byte>, JournalCodecError>;
using DecodedJournalFileHeader = std::variant<JournalFileHeader, JournalCodecError>;
using DecodedJournalRecordResult = std::variant<DecodedJournalRecord, JournalCodecError>;

[[nodiscard]] std::vector<std::byte> encode_file_header();
[[nodiscard]] DecodedJournalFileHeader decode_file_header(std::span<const std::byte> bytes);

[[nodiscard]] EncodedJournalRecord encode_record(
    JournalSequence sequence,
    const JournalCommand& command);

// Decodes one complete frame at the beginning of bytes and reports its exact size.
// Bytes after that frame are intentionally left for a future sequential reader.
[[nodiscard]] DecodedJournalRecordResult decode_record_prefix(std::span<const std::byte> bytes);

// Decodes exactly one frame. Additional bytes are rejected.
[[nodiscard]] DecodedJournalRecordResult decode_record(std::span<const std::byte> bytes);

[[nodiscard]] std::uint32_t crc32c(std::span<const std::byte> bytes) noexcept;

} // namespace matching
