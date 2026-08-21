#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <fcntl.h>
#include <sys/resource.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include "journal_storage_test_access.hpp"
#include "matching/durable_engine.hpp"
#include "matching/journal_storage.hpp"

namespace matching {
namespace {

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

[[nodiscard]] JournalCommand submit_command(
    std::uint64_t id,
    Side side,
    std::uint64_t price,
    std::uint64_t quantity) {
    return SubmitLimitOrderCommand{.order = order(id, side, price, quantity)};
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

void append_bytes(std::vector<std::byte>& destination, const std::vector<std::byte>& source) {
    destination.insert(destination.end(), source.begin(), source.end());
}

void write_all_or_fail(const std::filesystem::path& path, std::span<const std::byte> bytes) {
    const std::string filename = path.string();
    const int descriptor = ::open(filename.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    ASSERT_GE(descriptor, 0) << std::strerror(errno);

    std::size_t written = 0;
    while (written < bytes.size()) {
        const ssize_t result = ::write(descriptor, bytes.data() + written, bytes.size() - written);
        ASSERT_GT(result, 0) << std::strerror(errno);
        written += static_cast<std::size_t>(result);
    }
    ASSERT_EQ(::close(descriptor), 0) << std::strerror(errno);
}

[[nodiscard]] std::vector<std::byte> read_all_or_fail(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    EXPECT_TRUE(input.is_open());
    const std::vector<char> characters{
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()};
    std::vector<std::byte> bytes;
    bytes.reserve(characters.size());
    for (const char character : characters) {
        bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(character)));
    }
    return bytes;
}

void append_file_bytes_or_fail(const std::filesystem::path& path, std::span<const std::byte> bytes) {
    const std::string filename = path.string();
    const int descriptor = ::open(filename.c_str(), O_WRONLY | O_APPEND | O_CLOEXEC);
    ASSERT_GE(descriptor, 0) << std::strerror(errno);
    std::size_t written = 0;
    while (written < bytes.size()) {
        const ssize_t result = ::write(descriptor, bytes.data() + written, bytes.size() - written);
        ASSERT_GT(result, 0) << std::strerror(errno);
        written += static_cast<std::size_t>(result);
    }
    ASSERT_EQ(::close(descriptor), 0) << std::strerror(errno);
}

void expect_storage_error(
    const JournalScanResult& result,
    JournalStorageErrorCode expected_code) {
    ASSERT_TRUE(std::holds_alternative<JournalStorageError>(result));
    EXPECT_EQ(std::get<JournalStorageError>(result).code, expected_code);
}

template <typename Result>
void expect_storage_error(const Result& result, JournalStorageErrorCode expected_code) {
    ASSERT_TRUE(std::holds_alternative<JournalStorageError>(result));
    EXPECT_EQ(std::get<JournalStorageError>(result).code, expected_code);
}

class JournalStorageTest : public ::testing::Test {
protected:
    void SetUp() override {
        testing::JournalStorageTestAccess::reset_append_io();
        std::array<char, 64> template_path{};
        constexpr char kPattern[] = "/tmp/matching-engine-journal-XXXXXX";
        std::copy(std::begin(kPattern), std::end(kPattern), template_path.begin());
        char* directory = ::mkdtemp(template_path.data());
        ASSERT_NE(directory, nullptr) << std::strerror(errno);
        temporary_directory_ = directory;
        journal_path_ = temporary_directory_ / "journal.bin";
    }

    void TearDown() override {
        testing::JournalStorageTestAccess::reset_append_io();
        std::error_code error;
        std::filesystem::remove_all(temporary_directory_, error);
        EXPECT_FALSE(error);
    }

    std::filesystem::path temporary_directory_;
    std::filesystem::path journal_path_;
};

TEST_F(JournalStorageTest, CreatesFreshJournalWithoutOverwritingExistingPath) {
    auto create_result = JournalWriter::create(journal_path_);
    ASSERT_TRUE(std::holds_alternative<JournalWriter>(create_result));
    JournalWriter writer = std::move(std::get<JournalWriter>(create_result));

    EXPECT_EQ(read_all_or_fail(journal_path_), encode_file_header());
    ASSERT_TRUE(writer.next_sequence().has_value());
    EXPECT_EQ(*writer.next_sequence(), JournalSequence{1});

    const JournalScanResult scan_result = JournalReader::scan(journal_path_);
    ASSERT_TRUE(std::holds_alternative<JournalScan>(scan_result));
    const JournalScan& scan = std::get<JournalScan>(scan_result);
    EXPECT_TRUE(scan.records.empty());
    EXPECT_EQ(scan.valid_size, kJournalFileHeaderSize);
    EXPECT_FALSE(scan.has_truncated_tail);
    ASSERT_TRUE(scan.next_sequence.has_value());
    EXPECT_EQ(*scan.next_sequence, JournalSequence{1});

    const auto duplicate_create = JournalWriter::create(journal_path_);
    expect_storage_error(duplicate_create, JournalStorageErrorCode::FileAlreadyExists);
    EXPECT_EQ(read_all_or_fail(journal_path_), encode_file_header());
}

TEST_F(JournalStorageTest, AppendsFramesWithWriterOwnedConsecutiveSequences) {
    const JournalCommand submit = submit_command(10, Side::Buy, 100, 3);
    const JournalCommand cancel = CancelOrderCommand{.order_id = OrderId{10}};

    auto create_result = JournalWriter::create(journal_path_);
    ASSERT_TRUE(std::holds_alternative<JournalWriter>(create_result));
    JournalWriter writer = std::move(std::get<JournalWriter>(create_result));

    const JournalAppendResult first_append = writer.append_and_sync(submit);
    ASSERT_TRUE(std::holds_alternative<JournalSequence>(first_append));
    EXPECT_EQ(std::get<JournalSequence>(first_append), JournalSequence{1});
    const JournalAppendResult second_append = writer.append_and_sync(cancel);
    ASSERT_TRUE(std::holds_alternative<JournalSequence>(second_append));
    EXPECT_EQ(std::get<JournalSequence>(second_append), JournalSequence{2});

    std::vector<std::byte> expected = encode_file_header();
    append_bytes(expected, encoded_record_or_failure(JournalSequence{1}, submit));
    append_bytes(expected, encoded_record_or_failure(JournalSequence{2}, cancel));
    EXPECT_EQ(read_all_or_fail(journal_path_), expected);

    const JournalScanResult scan_result = JournalReader::scan(journal_path_);
    ASSERT_TRUE(std::holds_alternative<JournalScan>(scan_result));
    const JournalScan& scan = std::get<JournalScan>(scan_result);
    ASSERT_EQ(scan.records.size(), std::size_t{2});
    EXPECT_EQ(scan.records[0].sequence, JournalSequence{1});
    EXPECT_EQ(scan.records[1].sequence, JournalSequence{2});
    ASSERT_TRUE(std::holds_alternative<SubmitLimitOrderCommand>(scan.records[0].command));
    ASSERT_TRUE(std::holds_alternative<CancelOrderCommand>(scan.records[1].command));
    EXPECT_EQ(std::get<SubmitLimitOrderCommand>(scan.records[0].command).order.id, OrderId{10});
    EXPECT_EQ(std::get<CancelOrderCommand>(scan.records[1].command).order_id, OrderId{10});
    ASSERT_TRUE(scan.next_sequence.has_value());
    EXPECT_EQ(*scan.next_sequence, JournalSequence{3});
}

TEST_F(JournalStorageTest, ReopensCleanJournalAndContinuesSequence) {
    {
        auto create_result = JournalWriter::create(journal_path_);
        ASSERT_TRUE(std::holds_alternative<JournalWriter>(create_result));
        JournalWriter writer = std::move(std::get<JournalWriter>(create_result));
        ASSERT_TRUE(std::holds_alternative<JournalSequence>(
            writer.append_and_sync(submit_command(1, Side::Buy, 100, 1))));
        ASSERT_TRUE(std::holds_alternative<JournalSequence>(
            writer.append_and_sync(submit_command(2, Side::Sell, 101, 2))));
    }

    const std::vector<std::byte> bytes_before_reopen = read_all_or_fail(journal_path_);
    auto reopened_result = JournalWriter::open_recovered(journal_path_);
    ASSERT_TRUE(std::holds_alternative<JournalWriter>(reopened_result));
    JournalWriter writer = std::move(std::get<JournalWriter>(reopened_result));
    EXPECT_EQ(read_all_or_fail(journal_path_), bytes_before_reopen);
    ASSERT_TRUE(writer.next_sequence().has_value());
    EXPECT_EQ(*writer.next_sequence(), JournalSequence{3});
    const JournalAppendResult append = writer.append_and_sync(
        CancelOrderCommand{.order_id = OrderId{1}});
    ASSERT_TRUE(std::holds_alternative<JournalSequence>(append));
    EXPECT_EQ(std::get<JournalSequence>(append), JournalSequence{3});
}

TEST_F(JournalStorageTest, ScansManyDeterministicRecordsInOrder) {
    auto create_result = JournalWriter::create(journal_path_);
    ASSERT_TRUE(std::holds_alternative<JournalWriter>(create_result));
    JournalWriter writer = std::move(std::get<JournalWriter>(create_result));

    constexpr std::uint64_t kRecordCount = 20;
    for (std::uint64_t index = 1; index <= kRecordCount; ++index) {
        const JournalCommand command = index % 2 == 0
                                           ? JournalCommand{CancelOrderCommand{.order_id = OrderId{index}}}
                                           : submit_command(index, Side::Buy, 100 + index, index);
        const JournalAppendResult append = writer.append_and_sync(command);
        ASSERT_TRUE(std::holds_alternative<JournalSequence>(append));
        EXPECT_EQ(std::get<JournalSequence>(append), JournalSequence{index});
    }

    const JournalScanResult scan_result = JournalReader::scan(journal_path_);
    ASSERT_TRUE(std::holds_alternative<JournalScan>(scan_result));
    const JournalScan& scan = std::get<JournalScan>(scan_result);
    ASSERT_EQ(scan.records.size(), static_cast<std::size_t>(kRecordCount));
    for (std::uint64_t index = 1; index <= kRecordCount; ++index) {
        EXPECT_EQ(scan.records[index - 1U].sequence, JournalSequence{index});
    }
    ASSERT_TRUE(scan.next_sequence.has_value());
    EXPECT_EQ(*scan.next_sequence, JournalSequence{kRecordCount + 1U});
}

TEST_F(JournalStorageTest, RejectsNonConsecutiveOnDiskSequences) {
    const JournalCommand command = submit_command(1, Side::Buy, 100, 1);
    const std::vector<std::vector<JournalSequence>> invalid_sequences{
        {JournalSequence{2}},
        {JournalSequence{1}, JournalSequence{1}},
        {JournalSequence{1}, JournalSequence{3}},
        {JournalSequence{1}, JournalSequence{2}, JournalSequence{1}},
    };

    for (const auto& sequences : invalid_sequences) {
        std::vector<std::byte> bytes = encode_file_header();
        for (const JournalSequence sequence : sequences) {
            append_bytes(bytes, encoded_record_or_failure(sequence, command));
        }
        write_all_or_fail(journal_path_, bytes);
        expect_storage_error(JournalReader::scan(journal_path_), JournalStorageErrorCode::SequenceViolation);
    }
}

TEST_F(JournalStorageTest, TreatsEveryIncompleteFileHeaderAsFatalJournalCorruption) {
    const std::vector<std::byte> header = encode_file_header();

    for (std::size_t length = 0; length < header.size(); ++length) {
        write_all_or_fail(journal_path_, std::span<const std::byte>(header).first(length));
        const JournalScanResult scan_result = JournalReader::scan(journal_path_);
        ASSERT_TRUE(std::holds_alternative<JournalStorageError>(scan_result));
        const JournalStorageError& error = std::get<JournalStorageError>(scan_result);
        EXPECT_EQ(error.code, JournalStorageErrorCode::InvalidJournal);
        EXPECT_EQ(error.codec_error, JournalCodecError::TruncatedInput);
    }
}

TEST_F(JournalStorageTest, ReportsEachPartialFinalFrameAsRecoverableTail) {
    const JournalCommand first = submit_command(1, Side::Buy, 100, 1);
    const JournalCommand second = CancelOrderCommand{.order_id = OrderId{1}};
    const JournalCommand final = submit_command(2, Side::Sell, 101, 2);
    const std::vector<std::byte> first_frame = encoded_record_or_failure(JournalSequence{1}, first);
    const std::vector<std::byte> second_frame = encoded_record_or_failure(JournalSequence{2}, second);
    const std::vector<std::byte> final_frame = encoded_record_or_failure(JournalSequence{3}, final);
    std::vector<std::byte> prefix = encode_file_header();
    append_bytes(prefix, first_frame);
    append_bytes(prefix, second_frame);

    for (std::size_t length = 1; length < final_frame.size(); ++length) {
        std::vector<std::byte> bytes = prefix;
        bytes.insert(bytes.end(), final_frame.begin(), final_frame.begin() + static_cast<std::ptrdiff_t>(length));
        write_all_or_fail(journal_path_, bytes);

        const JournalScanResult scan_result = JournalReader::scan(journal_path_);
        ASSERT_TRUE(std::holds_alternative<JournalScan>(scan_result));
        const JournalScan& scan = std::get<JournalScan>(scan_result);
        EXPECT_EQ(scan.records.size(), std::size_t{2});
        EXPECT_EQ(scan.valid_size, prefix.size());
        EXPECT_TRUE(scan.has_truncated_tail);
        ASSERT_TRUE(scan.next_sequence.has_value());
        EXPECT_EQ(*scan.next_sequence, JournalSequence{3});
    }
}

TEST_F(JournalStorageTest, RepairsOnlyVerifiedTruncatedTailThenContinuesSequence) {
    const JournalCommand first = submit_command(1, Side::Buy, 100, 1);
    const JournalCommand second = CancelOrderCommand{.order_id = OrderId{1}};
    const JournalCommand partial = submit_command(2, Side::Sell, 101, 2);

    {
        auto create_result = JournalWriter::create(journal_path_);
        ASSERT_TRUE(std::holds_alternative<JournalWriter>(create_result));
        JournalWriter writer = std::move(std::get<JournalWriter>(create_result));
        ASSERT_TRUE(std::holds_alternative<JournalSequence>(writer.append_and_sync(first)));
        ASSERT_TRUE(std::holds_alternative<JournalSequence>(writer.append_and_sync(second)));
    }
    const std::vector<std::byte> partial_frame = encoded_record_or_failure(JournalSequence{3}, partial);
    append_file_bytes_or_fail(journal_path_, std::span<const std::byte>(partial_frame).first(9));

    const JournalScanResult before_repair = JournalReader::scan(journal_path_);
    ASSERT_TRUE(std::holds_alternative<JournalScan>(before_repair));
    EXPECT_TRUE(std::get<JournalScan>(before_repair).has_truncated_tail);
    const std::uint64_t valid_size = std::get<JournalScan>(before_repair).valid_size;

    const auto open_without_repair = JournalWriter::open_recovered(journal_path_);
    expect_storage_error(open_without_repair, JournalStorageErrorCode::TruncatedTailRequiresRepair);

    auto repair_result = JournalWriter::repair_truncated_tail_and_open(journal_path_);
    ASSERT_TRUE(std::holds_alternative<JournalWriter>(repair_result));
    JournalWriter writer = std::move(std::get<JournalWriter>(repair_result));
    EXPECT_EQ(std::filesystem::file_size(journal_path_), valid_size);
    ASSERT_TRUE(writer.next_sequence().has_value());
    EXPECT_EQ(*writer.next_sequence(), JournalSequence{3});

    const JournalAppendResult append = writer.append_and_sync(partial);
    ASSERT_TRUE(std::holds_alternative<JournalSequence>(append));
    EXPECT_EQ(std::get<JournalSequence>(append), JournalSequence{3});
    const JournalScanResult after_repair = JournalReader::scan(journal_path_);
    ASSERT_TRUE(std::holds_alternative<JournalScan>(after_repair));
    EXPECT_FALSE(std::get<JournalScan>(after_repair).has_truncated_tail);
    EXPECT_EQ(std::get<JournalScan>(after_repair).records.size(), std::size_t{3});
}

TEST_F(JournalStorageTest, DoesNotRepairCleanJournal) {
    auto create_result = JournalWriter::create(journal_path_);
    ASSERT_TRUE(std::holds_alternative<JournalWriter>(create_result));

    const auto repair_result = JournalWriter::repair_truncated_tail_and_open(journal_path_);
    expect_storage_error(repair_result, JournalStorageErrorCode::NoTruncatedTail);
}

TEST_F(JournalStorageTest, RejectsCompleteFrameCorruptionWithoutTreatingItAsTail) {
    const JournalCommand command = submit_command(1, Side::Buy, 100, 1);
    const std::vector<std::byte> valid_frame = encoded_record_or_failure(JournalSequence{1}, command);
    const std::array<std::size_t, 8> mutated_indices{0, 4, 5, 7, 11, 12, 24, 20};

    for (const std::size_t index : mutated_indices) {
        std::vector<std::byte> bytes = encode_file_header();
        std::vector<std::byte> corrupted_frame = valid_frame;
        corrupted_frame[index] ^= std::byte{1};
        append_bytes(bytes, corrupted_frame);
        write_all_or_fail(journal_path_, bytes);
        expect_storage_error(JournalReader::scan(journal_path_), JournalStorageErrorCode::InvalidJournal);
    }
}

TEST_F(JournalStorageTest, RejectsMiddleFrameCorruptionWithoutScanningPastIt) {
    const JournalCommand command = submit_command(1, Side::Buy, 100, 1);
    std::vector<std::byte> bytes = encode_file_header();
    append_bytes(bytes, encoded_record_or_failure(JournalSequence{1}, command));
    std::vector<std::byte> corrupted = encoded_record_or_failure(JournalSequence{2}, command);
    corrupted[20] ^= std::byte{1};
    append_bytes(bytes, corrupted);
    append_bytes(bytes, encoded_record_or_failure(JournalSequence{3}, command));
    write_all_or_fail(journal_path_, bytes);

    expect_storage_error(JournalReader::scan(journal_path_), JournalStorageErrorCode::InvalidJournal);
}

TEST_F(JournalStorageTest, FailedWriteDoesNotReportSuccessAndPermanentlyFailsWriter) {
    auto create_result = JournalWriter::create(journal_path_);
    ASSERT_TRUE(std::holds_alternative<JournalWriter>(create_result));
    JournalWriter writer = std::move(std::get<JournalWriter>(create_result));

    struct rlimit previous_limit {};
    ASSERT_EQ(::getrlimit(RLIMIT_FSIZE, &previous_limit), 0) << std::strerror(errno);
    struct sigaction previous_action {};
    struct sigaction ignored_action {};
    ignored_action.sa_handler = SIG_IGN;
    ASSERT_EQ(::sigemptyset(&ignored_action.sa_mask), 0) << std::strerror(errno);
    ASSERT_EQ(::sigaction(SIGXFSZ, &ignored_action, &previous_action), 0) << std::strerror(errno);

    struct rlimit constrained_limit = previous_limit;
    constrained_limit.rlim_cur = static_cast<rlim_t>(kJournalFileHeaderSize);
    ASSERT_EQ(::setrlimit(RLIMIT_FSIZE, &constrained_limit), 0) << std::strerror(errno);

    const JournalAppendResult failed_append = writer.append_and_sync(
        submit_command(1, Side::Buy, 100, 1));

    EXPECT_EQ(::setrlimit(RLIMIT_FSIZE, &previous_limit), 0) << std::strerror(errno);
    EXPECT_EQ(::sigaction(SIGXFSZ, &previous_action, nullptr), 0) << std::strerror(errno);

    expect_storage_error(failed_append, JournalStorageErrorCode::WriteFailed);
    EXPECT_TRUE(writer.failed());
    ASSERT_TRUE(writer.next_sequence().has_value());
    EXPECT_EQ(*writer.next_sequence(), JournalSequence{1});
    expect_storage_error(
        writer.append_and_sync(submit_command(1, Side::Buy, 100, 1)),
        JournalStorageErrorCode::WriterFailed);
}

TEST_F(JournalStorageTest, BatchAppendPreservesFrameOrderAndContinuesWithSingleAppend) {
    const std::array<JournalCommand, 3> batch{
        submit_command(1, Side::Buy, 100, 2),
        JournalCommand{CancelOrderCommand{.order_id = OrderId{1}}},
        submit_command(2, Side::Sell, 101, 3),
    };
    auto created = JournalWriter::create(journal_path_);
    ASSERT_TRUE(std::holds_alternative<JournalWriter>(created));
    JournalWriter writer = std::move(std::get<JournalWriter>(created));

    const JournalAppendBatchResult appended = writer.append_batch_and_sync(batch);
    ASSERT_TRUE(std::holds_alternative<std::vector<JournalSequence>>(appended));
    const auto& sequences = std::get<std::vector<JournalSequence>>(appended);
    ASSERT_EQ(sequences.size(), batch.size());
    EXPECT_EQ(sequences[0], JournalSequence{1});
    EXPECT_EQ(sequences[1], JournalSequence{2});
    EXPECT_EQ(sequences[2], JournalSequence{3});
    ASSERT_TRUE(std::holds_alternative<JournalSequence>(writer.append_and_sync(
        submit_command(3, Side::Buy, 99, 1))));

    const JournalScanResult scan_result = JournalReader::scan(journal_path_);
    ASSERT_TRUE(std::holds_alternative<JournalScan>(scan_result));
    const auto& records = std::get<JournalScan>(scan_result).records;
    ASSERT_EQ(records.size(), std::size_t{4});
    EXPECT_TRUE(std::holds_alternative<SubmitLimitOrderCommand>(records[0].command));
    EXPECT_TRUE(std::holds_alternative<CancelOrderCommand>(records[1].command));
    EXPECT_TRUE(std::holds_alternative<SubmitLimitOrderCommand>(records[2].command));
    EXPECT_EQ(std::get<SubmitLimitOrderCommand>(records[2].command).order.id, OrderId{2});
    EXPECT_EQ(records.back().sequence, JournalSequence{4});
}

TEST_F(JournalStorageTest, EmptyBatchWritesNothingAndDoesNotFailWriter) {
    auto created = JournalWriter::create(journal_path_);
    ASSERT_TRUE(std::holds_alternative<JournalWriter>(created));
    JournalWriter writer = std::move(std::get<JournalWriter>(created));
    const std::vector<JournalCommand> empty;
    const JournalAppendBatchResult result = writer.append_batch_and_sync(empty);
    expect_storage_error(result, JournalStorageErrorCode::EmptyBatch);
    EXPECT_FALSE(writer.failed());
    EXPECT_EQ(read_all_or_fail(journal_path_), encode_file_header());
}

TEST_F(JournalStorageTest, AppendFaultBoundariesLeaveOnlyTheActualValidPrefixForRecovery) {
    const std::array<JournalCommand, 3> commands{
        submit_command(1, Side::Buy, 101, 1),
        submit_command(2, Side::Buy, 102, 1),
        submit_command(3, Side::Buy, 103, 1),
    };
    const std::array<std::size_t, 3> frame_sizes{
        encoded_record_or_failure(JournalSequence{1}, commands[0]).size(),
        encoded_record_or_failure(JournalSequence{2}, commands[1]).size(),
        encoded_record_or_failure(JournalSequence{3}, commands[2]).size(),
    };
    struct Boundary {
        const char* name;
        std::size_t successful_bytes;
        std::size_t expected_records;
        bool expected_torn_tail;
    };
    const std::array boundaries{
        Boundary{"before_any_frame", 0U, 0U, false},
        Boundary{"inside_first_frame", frame_sizes[0] / 2U, 0U, true},
        Boundary{"after_one_complete_frame", frame_sizes[0], 1U, false},
        Boundary{"after_multiple_complete_frames", frame_sizes[0] + frame_sizes[1], 2U, false},
        Boundary{"inside_final_frame", frame_sizes[0] + frame_sizes[1] + frame_sizes[2] / 2U, 2U, true},
    };

    for (const Boundary& boundary : boundaries) {
        SCOPED_TRACE(boundary.name);
        const std::filesystem::path path = temporary_directory_ / (std::string(boundary.name) + ".bin");
        {
            auto created = JournalWriter::create(path);
            ASSERT_TRUE(std::holds_alternative<JournalWriter>(created));
            JournalWriter writer = std::move(std::get<JournalWriter>(created));
            testing::JournalStorageTestAccess::fail_append_write_after(boundary.successful_bytes);
            expect_storage_error(writer.append_batch_and_sync(commands), JournalStorageErrorCode::WriteFailed);
            EXPECT_TRUE(writer.failed());
            ASSERT_TRUE(writer.next_sequence().has_value());
            EXPECT_EQ(*writer.next_sequence(), JournalSequence{1});
            expect_storage_error(writer.append_and_sync(commands[0]), JournalStorageErrorCode::WriterFailed);
        }

        const JournalScanResult before_recovery = JournalReader::scan(path);
        ASSERT_TRUE(std::holds_alternative<JournalScan>(before_recovery));
        const JournalScan& scan = std::get<JournalScan>(before_recovery);
        EXPECT_EQ(scan.records.size(), boundary.expected_records);
        EXPECT_EQ(scan.has_truncated_tail, boundary.expected_torn_tail);

        auto recovered_result = DurableEngine::recover(path);
        ASSERT_TRUE(std::holds_alternative<DurableEngine>(recovered_result));
        DurableEngine recovered = std::move(std::get<DurableEngine>(recovered_result));
        EXPECT_EQ(recovered.order_book().active_order_count(), boundary.expected_records);
        const JournalScanResult repaired_scan = JournalReader::scan(path);
        ASSERT_TRUE(std::holds_alternative<JournalScan>(repaired_scan));
        EXPECT_FALSE(std::get<JournalScan>(repaired_scan).has_truncated_tail);
        EXPECT_EQ(std::get<JournalScan>(repaired_scan).records.size(), boundary.expected_records);
        testing::JournalStorageTestAccess::reset_append_io();
    }
}

TEST_F(JournalStorageTest, FsyncFailureAfterAllFramesLeavesCompleteUnacknowledgedBatchRecoverable) {
    const std::array<JournalCommand, 3> commands{
        submit_command(1, Side::Buy, 101, 1),
        submit_command(2, Side::Buy, 102, 1),
        submit_command(3, Side::Buy, 103, 1),
    };
    {
        auto created = JournalWriter::create(journal_path_);
        ASSERT_TRUE(std::holds_alternative<JournalWriter>(created));
        JournalWriter writer = std::move(std::get<JournalWriter>(created));
        testing::JournalStorageTestAccess::fail_next_append_sync();
        expect_storage_error(writer.append_batch_and_sync(commands), JournalStorageErrorCode::SyncFailed);
        EXPECT_TRUE(writer.failed());
        ASSERT_TRUE(writer.next_sequence().has_value());
        EXPECT_EQ(*writer.next_sequence(), JournalSequence{1});
        expect_storage_error(writer.append_and_sync(commands[0]), JournalStorageErrorCode::WriterFailed);
    }
    const JournalScanResult scan_result = JournalReader::scan(journal_path_);
    ASSERT_TRUE(std::holds_alternative<JournalScan>(scan_result));
    EXPECT_FALSE(std::get<JournalScan>(scan_result).has_truncated_tail);
    EXPECT_EQ(std::get<JournalScan>(scan_result).records.size(), commands.size());
    auto recovered_result = DurableEngine::recover(journal_path_);
    ASSERT_TRUE(std::holds_alternative<DurableEngine>(recovered_result));
    EXPECT_EQ(std::get<DurableEngine>(recovered_result).order_book().active_order_count(), commands.size());
}

TEST_F(JournalStorageTest, EveryNonEmptyBatchUsesExactlyOneAppendPathFsync) {
    auto created = JournalWriter::create(journal_path_);
    ASSERT_TRUE(std::holds_alternative<JournalWriter>(created));
    JournalWriter writer = std::move(std::get<JournalWriter>(created));
    testing::JournalStorageTestAccess::reset_append_io();
    const std::array<JournalCommand, 1> one{submit_command(1, Side::Buy, 100, 1)};
    const std::array<JournalCommand, 2> two{
        submit_command(2, Side::Buy, 101, 1), submit_command(3, Side::Buy, 102, 1)};
    std::array<JournalCommand, 8> eight{};
    for (std::size_t index = 0; index < eight.size(); ++index) {
        eight[index] = submit_command(4U + index, Side::Buy, 103U + index, 1);
    }
    ASSERT_TRUE(std::holds_alternative<std::vector<JournalSequence>>(writer.append_batch_and_sync(one)));
    EXPECT_EQ(testing::JournalStorageTestAccess::successful_append_sync_count(), 1U);
    ASSERT_TRUE(std::holds_alternative<std::vector<JournalSequence>>(writer.append_batch_and_sync(two)));
    EXPECT_EQ(testing::JournalStorageTestAccess::successful_append_sync_count(), 2U);
    ASSERT_TRUE(std::holds_alternative<std::vector<JournalSequence>>(writer.append_batch_and_sync(eight)));
    EXPECT_EQ(testing::JournalStorageTestAccess::successful_append_sync_count(), 3U);
}

} // namespace
} // namespace matching
