#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <system_error>
#include <variant>
#include <vector>

#include "matching/journal.hpp"

namespace matching {

enum class JournalStorageErrorCode : std::uint8_t {
    FileAlreadyExists,
    FileNotFound,
    OpenFailed,
    ReadFailed,
    WriteFailed,
    SyncFailed,
    DirectorySyncFailed,
    TruncateFailed,
    InvalidJournal,
    SequenceViolation,
    SequenceExhausted,
    TruncatedTailRequiresRepair,
    NoTruncatedTail,
    WriterFailed,
    CodecFailure,
    EmptyBatch,
};

struct JournalStorageError {
    JournalStorageErrorCode code;
    std::error_code system_error{};
    std::optional<JournalCodecError> codec_error{};
};

struct JournalScan {
    std::vector<JournalRecord> records;
    std::uint64_t valid_size{};
    bool has_truncated_tail{};
    // An empty value represents sequence exhaustion after a valid maximum-value record.
    std::optional<JournalSequence> next_sequence;
};

using JournalScanResult = std::variant<JournalScan, JournalStorageError>;
using JournalAppendResult = std::variant<JournalSequence, JournalStorageError>;
using JournalAppendBatchResult = std::variant<std::vector<JournalSequence>, JournalStorageError>;

class JournalReader {
public:
    [[nodiscard]] static JournalScanResult scan(const std::filesystem::path& path);
};

class JournalWriter {
public:
    JournalWriter(const JournalWriter&) = delete;
    JournalWriter& operator=(const JournalWriter&) = delete;
    JournalWriter(JournalWriter&& other) noexcept;
    JournalWriter& operator=(JournalWriter&& other) noexcept;
    ~JournalWriter();

    // Creates a new file exclusively. An existing path is never overwritten.
    [[nodiscard]] static std::variant<JournalWriter, JournalStorageError> create(
        const std::filesystem::path& path);

    // Scans the opened descriptor internally and refuses journals with a truncated tail.
    [[nodiscard]] static std::variant<JournalWriter, JournalStorageError> open_recovered(
        const std::filesystem::path& path);

    // Rescans internally, repairs only a verified incomplete final frame, then opens for append.
    [[nodiscard]] static std::variant<JournalWriter, JournalStorageError>
    repair_truncated_tail_and_open(const std::filesystem::path& path);

    // A failed write or sync permanently fails this writer. Reopen through recovery before use.
    [[nodiscard]] JournalAppendResult append_and_sync(const JournalCommand& command);

    // Prepares every frame, writes them in order, then performs exactly one fsync.
    // A failed write or sync permanently fails this writer.
    [[nodiscard]] JournalAppendBatchResult append_batch_and_sync(
        std::span<const JournalCommand> commands);

    [[nodiscard]] std::optional<JournalSequence> next_sequence() const noexcept;
    [[nodiscard]] bool failed() const noexcept;

private:
    JournalWriter(int file_descriptor, std::optional<JournalSequence> next_sequence) noexcept;

    int file_descriptor_{-1};
    std::optional<JournalSequence> next_sequence_;
    bool failed_{};
};

} // namespace matching
