#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include "matching/durable_engine.hpp"
#include "order_book_test_access.hpp"

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

void expect_quote_equal(const std::optional<Quote>& left, const std::optional<Quote>& right) {
    ASSERT_EQ(left.has_value(), right.has_value());
    if (left.has_value()) {
        EXPECT_EQ(left->price, right->price);
        EXPECT_EQ(left->quantity, right->quantity);
    }
}

void expect_books_equal(
    const OrderBook& expected,
    const OrderBook& actual,
    const std::vector<OrderId>& known_ids) {
    expect_quote_equal(expected.best_bid(), actual.best_bid());
    expect_quote_equal(expected.best_ask(), actual.best_ask());
    EXPECT_EQ(expected.active_order_count(), actual.active_order_count());

    for (const Side side : {Side::Buy, Side::Sell}) {
        const std::vector<PriceLevelView> expected_depth = expected.depth(side, 10'000);
        const std::vector<PriceLevelView> actual_depth = actual.depth(side, 10'000);
        ASSERT_EQ(expected_depth.size(), actual_depth.size());
        for (std::size_t index = 0; index < expected_depth.size(); ++index) {
            EXPECT_EQ(expected_depth[index].price, actual_depth[index].price);
            EXPECT_EQ(expected_depth[index].total_quantity, actual_depth[index].total_quantity);
            EXPECT_EQ(expected_depth[index].order_count, actual_depth[index].order_count);
        }
    }

    for (const OrderId id : known_ids) {
        const auto expected_order = expected.find(id);
        const auto actual_order = actual.find(id);
        ASSERT_EQ(expected_order.has_value(), actual_order.has_value());
        if (expected_order.has_value()) {
            EXPECT_EQ(expected_order->id, actual_order->id);
            EXPECT_EQ(expected_order->side, actual_order->side);
            EXPECT_EQ(expected_order->limit_price, actual_order->limit_price);
            EXPECT_EQ(expected_order->original_quantity, actual_order->original_quantity);
            EXPECT_EQ(expected_order->remaining_quantity, actual_order->remaining_quantity);
        }
    }

    EXPECT_TRUE(testing::OrderBookTestAccess::invariants_hold(expected));
    EXPECT_TRUE(testing::OrderBookTestAccess::invariants_hold(actual));
}

void expect_trades_equal(const std::vector<Trade>& expected, const std::vector<Trade>& actual) {
    ASSERT_EQ(expected.size(), actual.size());
    for (std::size_t index = 0; index < expected.size(); ++index) {
        EXPECT_EQ(expected[index].id, actual[index].id);
        EXPECT_EQ(expected[index].maker_order_id, actual[index].maker_order_id);
        EXPECT_EQ(expected[index].taker_order_id, actual[index].taker_order_id);
        EXPECT_EQ(expected[index].execution_price, actual[index].execution_price);
        EXPECT_EQ(expected[index].execution_quantity, actual[index].execution_quantity);
    }
}

void apply_and_compare(DurableEngine& engine, OrderBook& reference, const JournalCommand& command) {
    std::visit(
        [&engine, &reference](const auto& typed_command) {
            using Command = std::decay_t<decltype(typed_command)>;
            if constexpr (std::is_same_v<Command, SubmitLimitOrderCommand>) {
                const SubmitResult expected = reference.submit(typed_command.order);
                const auto actual_result = engine.submit(typed_command.order);
                ASSERT_TRUE(std::holds_alternative<SubmitResult>(actual_result));
                const SubmitResult& actual = std::get<SubmitResult>(actual_result);
                EXPECT_EQ(actual.status, expected.status);
                EXPECT_EQ(actual.rejection_reason, expected.rejection_reason);
                EXPECT_EQ(actual.executed_quantity, expected.executed_quantity);
                EXPECT_EQ(actual.resting_quantity, expected.resting_quantity);
                expect_trades_equal(expected.trades, actual.trades);
            } else {
                const CancelResult expected = reference.cancel(typed_command.order_id);
                const auto actual_result = engine.cancel(typed_command.order_id);
                ASSERT_TRUE(std::holds_alternative<CancelResult>(actual_result));
                const CancelResult& actual = std::get<CancelResult>(actual_result);
                EXPECT_EQ(actual.status, expected.status);
                EXPECT_EQ(actual.cancelled_quantity, expected.cancelled_quantity);
            }
        },
        command);
}

void apply_to_reference(OrderBook& reference, const JournalCommand& command) {
    std::visit(
        [&reference](const auto& typed_command) {
            using Command = std::decay_t<decltype(typed_command)>;
            if constexpr (std::is_same_v<Command, SubmitLimitOrderCommand>) {
                static_cast<void>(reference.submit(typed_command.order));
            } else {
                static_cast<void>(reference.cancel(typed_command.order_id));
            }
        },
        command);
}

[[nodiscard]] std::vector<std::byte> encode_or_fail(
    JournalSequence sequence,
    const JournalCommand& command) {
    const EncodedJournalRecord encoded = encode_record(sequence, command);
    if (const auto* bytes = std::get_if<std::vector<std::byte>>(&encoded)) {
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

[[nodiscard]] std::vector<JournalCommand> representative_trace() {
    return {
        SubmitLimitOrderCommand{.order = order(1, Side::Buy, 100, 5)},
        SubmitLimitOrderCommand{.order = order(2, Side::Buy, 99, 3)},
        SubmitLimitOrderCommand{.order = order(3, Side::Sell, 100, 2)},
        CancelOrderCommand{.order_id = OrderId{2}},
        SubmitLimitOrderCommand{.order = order(1, Side::Sell, 101, 1)},
        SubmitLimitOrderCommand{.order = order(4, Side::Buy, 0, 1)},
        CancelOrderCommand{.order_id = OrderId{999}},
        SubmitLimitOrderCommand{.order = order(5, Side::Sell, 99, 4)},
        SubmitLimitOrderCommand{.order = order(6, Side::Buy, 101, 3)},
        CancelOrderCommand{.order_id = OrderId{3}},
        SubmitLimitOrderCommand{.order = order(7, static_cast<Side>(0xA5U), 100, 1)},
        SubmitLimitOrderCommand{.order = order(8, Side::Buy, 97, 2)},
        SubmitLimitOrderCommand{.order = order(9, Side::Sell, 97, 1)},
        CancelOrderCommand{.order_id = OrderId{8}},
    };
}

class DurableEngineStressTest : public ::testing::Test {
protected:
    void SetUp() override {
        std::array<char, 64> template_path{};
        constexpr char kPattern[] = "/tmp/matching-engine-stress-XXXXXX";
        std::copy(std::begin(kPattern), std::end(kPattern), template_path.begin());
        char* directory = ::mkdtemp(template_path.data());
        ASSERT_NE(directory, nullptr) << std::strerror(errno);
        temporary_directory_ = directory;
        journal_path_ = temporary_directory_ / "journal.bin";
    }

    void TearDown() override {
        std::error_code error;
        std::filesystem::remove_all(temporary_directory_, error);
        EXPECT_FALSE(error);
    }

    std::filesystem::path temporary_directory_;
    std::filesystem::path journal_path_;
};

TEST_F(DurableEngineStressTest, EveryCompleteJournalPrefixRecoversAsAuthoritativeState) {
    const std::vector<JournalCommand> trace = representative_trace();
    const std::vector<OrderId> known_ids{
        OrderId{0}, OrderId{1}, OrderId{2}, OrderId{3}, OrderId{4},
        OrderId{5}, OrderId{6}, OrderId{7}, OrderId{8}, OrderId{9}};
    std::vector<std::byte> bytes = encode_file_header();
    OrderBook expected;

    for (std::size_t prefix_length = 0; prefix_length <= trace.size(); ++prefix_length) {
        write_all_or_fail(journal_path_, bytes);
        {
            const std::uintmax_t size_before_recovery = std::filesystem::file_size(journal_path_);
            auto recovery_result = DurableEngine::recover(journal_path_);
            ASSERT_TRUE(std::holds_alternative<DurableEngine>(recovery_result));
            DurableEngine recovered = std::move(std::get<DurableEngine>(recovery_result));
            expect_books_equal(expected, recovered.order_book(), known_ids);
            EXPECT_EQ(std::filesystem::file_size(journal_path_), size_before_recovery);

            const JournalScanResult scan_result = JournalReader::scan(journal_path_);
            ASSERT_TRUE(std::holds_alternative<JournalScan>(scan_result));
            ASSERT_EQ(std::get<JournalScan>(scan_result).records.size(), prefix_length);
            ASSERT_TRUE(std::get<JournalScan>(scan_result).next_sequence.has_value());
            EXPECT_EQ(
                *std::get<JournalScan>(scan_result).next_sequence,
                JournalSequence{static_cast<std::uint64_t>(prefix_length + 1U)});

            if (prefix_length < trace.size()) {
                apply_and_compare(recovered, expected, trace[prefix_length]);
            }
        }

        if (prefix_length < trace.size()) {
            append_bytes(
                bytes,
                encode_or_fail(JournalSequence{static_cast<std::uint64_t>(prefix_length + 1U)}, trace[prefix_length]));
        }
    }
}

TEST_F(DurableEngineStressTest, FinalFrameTruncationMatrixRepairsThroughDurableEngine) {
    const JournalCommand first = SubmitLimitOrderCommand{.order = order(1, Side::Buy, 100, 3)};
    const JournalCommand second = SubmitLimitOrderCommand{.order = order(2, Side::Sell, 105, 2)};
    const JournalCommand final = SubmitLimitOrderCommand{.order = order(3, Side::Buy, 99, 1)};
    std::vector<std::byte> prefix = encode_file_header();
    append_bytes(prefix, encode_or_fail(JournalSequence{1}, first));
    append_bytes(prefix, encode_or_fail(JournalSequence{2}, second));
    const std::vector<std::byte> final_frame = encode_or_fail(JournalSequence{3}, final);
    for (std::size_t truncated_length = 1; truncated_length < final_frame.size(); ++truncated_length) {
        OrderBook expected;
        apply_to_reference(expected, first);
        apply_to_reference(expected, second);
        std::vector<std::byte> bytes = prefix;
        bytes.insert(
            bytes.end(),
            final_frame.begin(),
            final_frame.begin() + static_cast<std::ptrdiff_t>(truncated_length));
        write_all_or_fail(journal_path_, bytes);

        auto recovery_result = DurableEngine::recover(journal_path_);
        ASSERT_TRUE(std::holds_alternative<DurableEngine>(recovery_result));
        DurableEngine recovered = std::move(std::get<DurableEngine>(recovery_result));
        expect_books_equal(expected, recovered.order_book(), {OrderId{1}, OrderId{2}, OrderId{3}});
        EXPECT_EQ(std::filesystem::file_size(journal_path_), prefix.size());

        apply_and_compare(recovered, expected, final);
        const JournalScanResult scan_result = JournalReader::scan(journal_path_);
        ASSERT_TRUE(std::holds_alternative<JournalScan>(scan_result));
        ASSERT_EQ(std::get<JournalScan>(scan_result).records.size(), std::size_t{3});
        EXPECT_EQ(std::get<JournalScan>(scan_result).records.back().sequence, JournalSequence{3});
    }
}

TEST_F(DurableEngineStressTest, CompleteCorruptionIsFatalAndNeverRepairsTheJournal) {
    const JournalCommand first = SubmitLimitOrderCommand{.order = order(1, Side::Buy, 100, 1)};
    const JournalCommand second = CancelOrderCommand{.order_id = OrderId{1}};
    std::vector<std::byte> valid = encode_file_header();
    append_bytes(valid, encode_or_fail(JournalSequence{1}, first));
    append_bytes(valid, encode_or_fail(JournalSequence{2}, second));
    const std::array<std::size_t, 5> corruption_offsets{
        kJournalFileHeaderSize + kJournalFrameHeaderSize,
        kJournalFileHeaderSize + 20U,
        kJournalFileHeaderSize + 12U,
        kJournalFileHeaderSize + 5U,
        0U,
    };

    for (const std::size_t offset : corruption_offsets) {
        std::vector<std::byte> corrupted = valid;
        corrupted[offset] ^= std::byte{1};
        write_all_or_fail(journal_path_, corrupted);

        const auto recovery_result = DurableEngine::recover(journal_path_);
        ASSERT_TRUE(std::holds_alternative<DurableEngineError>(recovery_result));
        EXPECT_EQ(std::get<DurableEngineError>(recovery_result).code, DurableEngineErrorCode::JournalFailure);
        EXPECT_EQ(read_all_or_fail(journal_path_), corrupted);
    }
}

TEST_F(DurableEngineStressTest, LongMixedHistorySurvivesSixRecoveryCheckpoints) {
    constexpr std::size_t kBlocks = 450;
    constexpr std::size_t kOperationsPerBlock = 7;
    constexpr std::size_t kCheckpointInterval = 75;
    OrderBook reference;
    std::vector<OrderId> known_ids;
    known_ids.reserve(kBlocks * 4U);

    auto create_result = DurableEngine::create(journal_path_);
    ASSERT_TRUE(std::holds_alternative<DurableEngine>(create_result));
    std::optional<DurableEngine> engine;
    engine.emplace(std::move(std::get<DurableEngine>(create_result)));
    std::uint64_t next_id = 1;
    std::size_t operation_count = 0;

    for (std::size_t block = 0; block < kBlocks; ++block) {
        const std::uint64_t price = 100U + static_cast<std::uint64_t>(block % 7U);
        const OrderId maker_id{next_id++};
        const OrderId taker_id{next_id++};
        const OrderId passive_id{next_id++};
        const OrderId invalid_id{next_id++};
        known_ids.insert(known_ids.end(), {maker_id, taker_id, passive_id, invalid_id});

        const std::array<JournalCommand, kOperationsPerBlock> commands{
            SubmitLimitOrderCommand{.order = order(
                maker_id.value, Side::Sell, price, 2U + static_cast<std::uint64_t>(block % 3U))},
            SubmitLimitOrderCommand{.order = order(
                taker_id.value, Side::Buy, price, 1U + static_cast<std::uint64_t>(block % 4U))},
            SubmitLimitOrderCommand{.order = order(passive_id.value, Side::Buy, 50, 1)},
            CancelOrderCommand{.order_id = passive_id},
            SubmitLimitOrderCommand{.order = order(maker_id.value, Side::Sell, 110, 1)},
            SubmitLimitOrderCommand{.order = order(invalid_id.value, Side::Buy, 0, 1)},
            CancelOrderCommand{.order_id = OrderId{10'000'000U + static_cast<std::uint64_t>(block)}},
        };

        for (const JournalCommand& command : commands) {
            apply_and_compare(*engine, reference, command);
            ++operation_count;
        }

        if ((block + 1U) % kCheckpointInterval == 0U) {
            const std::uintmax_t size_before_recovery = std::filesystem::file_size(journal_path_);
            engine.reset();
            auto recovery_result = DurableEngine::recover(journal_path_);
            ASSERT_TRUE(std::holds_alternative<DurableEngine>(recovery_result));
            engine.emplace(std::move(std::get<DurableEngine>(recovery_result)));
            expect_books_equal(reference, engine->order_book(), known_ids);
            EXPECT_EQ(std::filesystem::file_size(journal_path_), size_before_recovery);

            const JournalScanResult scan_result = JournalReader::scan(journal_path_);
            ASSERT_TRUE(std::holds_alternative<JournalScan>(scan_result));
            const JournalScan& scan = std::get<JournalScan>(scan_result);
            EXPECT_EQ(scan.records.size(), operation_count);
            ASSERT_TRUE(scan.next_sequence.has_value());
            EXPECT_EQ(*scan.next_sequence, JournalSequence{static_cast<std::uint64_t>(operation_count + 1U)});
        }
    }

    EXPECT_EQ(operation_count, kBlocks * kOperationsPerBlock);
    expect_books_equal(reference, engine->order_book(), known_ids);
}

} // namespace
} // namespace matching
