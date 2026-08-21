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
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <fcntl.h>
#include <sys/resource.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include "durable_engine_test_access.hpp"
#include "journal_storage_test_access.hpp"
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

void expect_book_equal(
    const OrderBook& left,
    const OrderBook& right,
    const std::vector<OrderId>& relevant_ids) {
    expect_quote_equal(left.best_bid(), right.best_bid());
    expect_quote_equal(left.best_ask(), right.best_ask());
    EXPECT_EQ(left.active_order_count(), right.active_order_count());

    for (const Side side : {Side::Buy, Side::Sell}) {
        const std::vector<PriceLevelView> left_depth = left.depth(side, 100);
        const std::vector<PriceLevelView> right_depth = right.depth(side, 100);
        ASSERT_EQ(left_depth.size(), right_depth.size());
        for (std::size_t index = 0; index < left_depth.size(); ++index) {
            EXPECT_EQ(left_depth[index].price, right_depth[index].price);
            EXPECT_EQ(left_depth[index].total_quantity, right_depth[index].total_quantity);
            EXPECT_EQ(left_depth[index].order_count, right_depth[index].order_count);
        }
    }

    for (const OrderId id : relevant_ids) {
        const auto left_order = left.find(id);
        const auto right_order = right.find(id);
        ASSERT_EQ(left_order.has_value(), right_order.has_value());
        if (left_order.has_value()) {
            EXPECT_EQ(left_order->id, right_order->id);
            EXPECT_EQ(left_order->side, right_order->side);
            EXPECT_EQ(left_order->limit_price, right_order->limit_price);
            EXPECT_EQ(left_order->original_quantity, right_order->original_quantity);
            EXPECT_EQ(left_order->remaining_quantity, right_order->remaining_quantity);
        }
    }

    EXPECT_TRUE(testing::OrderBookTestAccess::invariants_hold(left));
    EXPECT_TRUE(testing::OrderBookTestAccess::invariants_hold(right));
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

void write_all_or_fail(const std::filesystem::path& path, std::span<const std::byte> bytes) {
    const std::string filename = path.string();
    const int descriptor = ::open(filename.c_str(), O_WRONLY | O_TRUNC | O_CLOEXEC);
    ASSERT_GE(descriptor, 0) << std::strerror(errno);
    std::size_t written = 0;
    while (written < bytes.size()) {
        const ssize_t result = ::write(descriptor, bytes.data() + written, bytes.size() - written);
        ASSERT_GT(result, 0) << std::strerror(errno);
        written += static_cast<std::size_t>(result);
    }
    ASSERT_EQ(::close(descriptor), 0) << std::strerror(errno);
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

class DurableEngineTest : public ::testing::Test {
protected:
    void SetUp() override {
        testing::DurableEngineTestAccess::clear_before_batch_apply_hook();
        testing::JournalStorageTestAccess::reset_append_io();
        std::array<char, 64> template_path{};
        constexpr char kPattern[] = "/tmp/matching-engine-durable-XXXXXX";
        std::copy(std::begin(kPattern), std::end(kPattern), template_path.begin());
        char* directory = ::mkdtemp(template_path.data());
        ASSERT_NE(directory, nullptr) << std::strerror(errno);
        temporary_directory_ = directory;
        journal_path_ = temporary_directory_ / "journal.bin";
    }

    void TearDown() override {
        testing::DurableEngineTestAccess::clear_before_batch_apply_hook();
        testing::JournalStorageTestAccess::reset_append_io();
        std::error_code error;
        std::filesystem::remove_all(temporary_directory_, error);
        EXPECT_FALSE(error);
    }

    std::filesystem::path temporary_directory_;
    std::filesystem::path journal_path_;
};

TEST_F(DurableEngineTest, CreatesEmptyEngineAndFirstCommandUsesSequenceOne) {
    auto create_result = DurableEngine::create(journal_path_);
    ASSERT_TRUE(std::holds_alternative<DurableEngine>(create_result));
    DurableEngine engine = std::move(std::get<DurableEngine>(create_result));
    EXPECT_EQ(engine.state(), DurableEngineState::Healthy);
    EXPECT_EQ(engine.order_book().active_order_count(), std::size_t{0});

    const auto submit_result = engine.submit(order(1, Side::Buy, 100, 3));
    ASSERT_TRUE(std::holds_alternative<SubmitResult>(submit_result));
    EXPECT_TRUE(std::get<SubmitResult>(submit_result).accepted());

    const JournalScanResult scan_result = JournalReader::scan(journal_path_);
    ASSERT_TRUE(std::holds_alternative<JournalScan>(scan_result));
    const JournalScan& scan = std::get<JournalScan>(scan_result);
    ASSERT_EQ(scan.records.size(), std::size_t{1});
    EXPECT_EQ(scan.records.front().sequence, JournalSequence{1});
}

TEST_F(DurableEngineTest, RecoversPassiveStateWithoutGrowingCleanJournal) {
    std::vector<std::byte> bytes_before_recovery;
    {
        auto create_result = DurableEngine::create(journal_path_);
        ASSERT_TRUE(std::holds_alternative<DurableEngine>(create_result));
        DurableEngine engine = std::move(std::get<DurableEngine>(create_result));
        ASSERT_TRUE(std::holds_alternative<SubmitResult>(engine.submit(order(1, Side::Buy, 100, 3))));
        ASSERT_TRUE(std::holds_alternative<SubmitResult>(engine.submit(order(2, Side::Sell, 105, 4))));
        bytes_before_recovery = read_all_or_fail(journal_path_);
    }

    auto recovery_result = DurableEngine::recover(journal_path_);
    ASSERT_TRUE(std::holds_alternative<DurableEngine>(recovery_result));
    DurableEngine recovered = std::move(std::get<DurableEngine>(recovery_result));
    EXPECT_EQ(read_all_or_fail(journal_path_), bytes_before_recovery);
    ASSERT_TRUE(recovered.order_book().best_bid().has_value());
    ASSERT_TRUE(recovered.order_book().best_ask().has_value());
    EXPECT_EQ(recovered.order_book().best_bid()->price, Price{100});
    EXPECT_EQ(recovered.order_book().best_ask()->price, Price{105});
    EXPECT_EQ(recovered.order_book().find(OrderId{1})->remaining_quantity, Quantity{3});
    EXPECT_EQ(recovered.order_book().find(OrderId{2})->remaining_quantity, Quantity{4});
}

TEST_F(DurableEngineTest, RecoversMatchesCancellationsAndLifetimeOrderIds) {
    OrderBook expected;
    {
        auto create_result = DurableEngine::create(journal_path_);
        ASSERT_TRUE(std::holds_alternative<DurableEngine>(create_result));
        DurableEngine original = std::move(std::get<DurableEngine>(create_result));
        ASSERT_TRUE(std::holds_alternative<SubmitResult>(original.submit(order(1, Side::Sell, 100, 5))));
        ASSERT_TRUE(std::holds_alternative<SubmitResult>(original.submit(order(2, Side::Sell, 99, 2))));
        const auto match_result = original.submit(order(3, Side::Buy, 100, 4));
        ASSERT_TRUE(std::holds_alternative<SubmitResult>(match_result));
        ASSERT_EQ(std::get<SubmitResult>(match_result).trades.size(), std::size_t{2});
        ASSERT_TRUE(std::holds_alternative<CancelResult>(original.cancel(OrderId{1})));

        static_cast<void>(expected.submit(order(1, Side::Sell, 100, 5)));
        static_cast<void>(expected.submit(order(2, Side::Sell, 99, 2)));
        static_cast<void>(expected.submit(order(3, Side::Buy, 100, 4)));
        static_cast<void>(expected.cancel(OrderId{1}));
    }

    auto recovery_result = DurableEngine::recover(journal_path_);
    ASSERT_TRUE(std::holds_alternative<DurableEngine>(recovery_result));
    DurableEngine recovered = std::move(std::get<DurableEngine>(recovery_result));
    expect_book_equal(expected, recovered.order_book(), {OrderId{1}, OrderId{2}, OrderId{3}});

    const auto reused = recovered.submit(order(1, Side::Buy, 90, 1));
    ASSERT_TRUE(std::holds_alternative<SubmitResult>(reused));
    EXPECT_EQ(std::get<SubmitResult>(reused).rejection_reason, RejectionReason::DuplicateOrderId);
}

TEST_F(DurableEngineTest, PersistsRejectedCommandsAndTheirSequences) {
    OrderBook expected;
    {
        auto create_result = DurableEngine::create(journal_path_);
        ASSERT_TRUE(std::holds_alternative<DurableEngine>(create_result));
        DurableEngine engine = std::move(std::get<DurableEngine>(create_result));

        ASSERT_TRUE(std::holds_alternative<SubmitResult>(engine.submit(order(1, Side::Buy, 100, 1))));
        const auto duplicate = engine.submit(order(1, Side::Sell, 101, 1));
        const auto zero_price = engine.submit(order(2, Side::Buy, 0, 1));
        const auto zero_quantity = engine.submit(order(3, Side::Buy, 100, 0));
        const auto zero_id = engine.submit(order(0, Side::Buy, 100, 1));
        const auto invalid_side = engine.submit(order(4, static_cast<Side>(0xA5U), 100, 1));
        const auto unknown_cancel = engine.cancel(OrderId{999});
        ASSERT_TRUE(std::holds_alternative<SubmitResult>(engine.submit(order(5, Side::Sell, 105, 2))));

        ASSERT_TRUE(std::holds_alternative<SubmitResult>(duplicate));
        EXPECT_EQ(std::get<SubmitResult>(duplicate).rejection_reason, RejectionReason::DuplicateOrderId);
        ASSERT_TRUE(std::holds_alternative<SubmitResult>(zero_price));
        EXPECT_EQ(std::get<SubmitResult>(zero_price).rejection_reason, RejectionReason::InvalidPrice);
        ASSERT_TRUE(std::holds_alternative<SubmitResult>(zero_quantity));
        EXPECT_EQ(std::get<SubmitResult>(zero_quantity).rejection_reason, RejectionReason::InvalidQuantity);
        ASSERT_TRUE(std::holds_alternative<SubmitResult>(zero_id));
        EXPECT_EQ(std::get<SubmitResult>(zero_id).rejection_reason, RejectionReason::InvalidOrderId);
        ASSERT_TRUE(std::holds_alternative<SubmitResult>(invalid_side));
        EXPECT_EQ(std::get<SubmitResult>(invalid_side).rejection_reason, RejectionReason::InvalidSide);
        ASSERT_TRUE(std::holds_alternative<CancelResult>(unknown_cancel));
        EXPECT_EQ(std::get<CancelResult>(unknown_cancel).status, CancelStatus::NotFound);

        static_cast<void>(expected.submit(order(1, Side::Buy, 100, 1)));
        static_cast<void>(expected.submit(order(1, Side::Sell, 101, 1)));
        static_cast<void>(expected.submit(order(2, Side::Buy, 0, 1)));
        static_cast<void>(expected.submit(order(3, Side::Buy, 100, 0)));
        static_cast<void>(expected.submit(order(0, Side::Buy, 100, 1)));
        static_cast<void>(expected.submit(order(4, static_cast<Side>(0xA5U), 100, 1)));
        static_cast<void>(expected.cancel(OrderId{999}));
        static_cast<void>(expected.submit(order(5, Side::Sell, 105, 2)));
    }

    auto recovery_result = DurableEngine::recover(journal_path_);
    ASSERT_TRUE(std::holds_alternative<DurableEngine>(recovery_result));
    DurableEngine recovered = std::move(std::get<DurableEngine>(recovery_result));
    expect_book_equal(expected, recovered.order_book(), {OrderId{0}, OrderId{1}, OrderId{2}, OrderId{3}, OrderId{4}, OrderId{5}});

    const auto next = recovered.submit(order(6, Side::Buy, 99, 1));
    ASSERT_TRUE(std::holds_alternative<SubmitResult>(next));
    const JournalScanResult scan_result = JournalReader::scan(journal_path_);
    ASSERT_TRUE(std::holds_alternative<JournalScan>(scan_result));
    EXPECT_EQ(std::get<JournalScan>(scan_result).records.size(), std::size_t{9});
    EXPECT_EQ(std::get<JournalScan>(scan_result).records.back().sequence, JournalSequence{9});
}

TEST_F(DurableEngineTest, RecoveryPreservesTradeIdsAndFifoMakerOrder) {
    const std::filesystem::path uninterrupted_path = temporary_directory_ / "uninterrupted.bin";
    const std::filesystem::path recovered_path = temporary_directory_ / "recovered.bin";
    auto uninterrupted_created = DurableEngine::create(uninterrupted_path);
    auto recovered_created = DurableEngine::create(recovered_path);
    ASSERT_TRUE(std::holds_alternative<DurableEngine>(uninterrupted_created));
    ASSERT_TRUE(std::holds_alternative<DurableEngine>(recovered_created));
    DurableEngine uninterrupted = std::move(std::get<DurableEngine>(uninterrupted_created));
    {
        DurableEngine prefix = std::move(std::get<DurableEngine>(recovered_created));
        for (const std::uint64_t id : {1ULL, 2ULL, 3ULL}) {
            ASSERT_TRUE(std::holds_alternative<SubmitResult>(
                uninterrupted.submit(order(id, Side::Buy, 100, id))));
            ASSERT_TRUE(std::holds_alternative<SubmitResult>(
                prefix.submit(order(id, Side::Buy, 100, id))));
        }
    }

    auto recovery_result = DurableEngine::recover(recovered_path);
    ASSERT_TRUE(std::holds_alternative<DurableEngine>(recovery_result));
    DurableEngine recovered = std::move(std::get<DurableEngine>(recovery_result));
    const auto uninterrupted_result = uninterrupted.submit(order(4, Side::Sell, 100, 6));
    const auto recovered_result = recovered.submit(order(4, Side::Sell, 100, 6));
    ASSERT_TRUE(std::holds_alternative<SubmitResult>(uninterrupted_result));
    ASSERT_TRUE(std::holds_alternative<SubmitResult>(recovered_result));
    const std::vector<Trade>& uninterrupted_trades = std::get<SubmitResult>(uninterrupted_result).trades;
    const std::vector<Trade>& recovered_trades = std::get<SubmitResult>(recovered_result).trades;
    ASSERT_EQ(uninterrupted_trades.size(), std::size_t{3});
    ASSERT_EQ(recovered_trades.size(), uninterrupted_trades.size());
    for (std::size_t index = 0; index < uninterrupted_trades.size(); ++index) {
        EXPECT_EQ(recovered_trades[index].id, uninterrupted_trades[index].id);
        EXPECT_EQ(recovered_trades[index].maker_order_id, uninterrupted_trades[index].maker_order_id);
        EXPECT_EQ(recovered_trades[index].taker_order_id, uninterrupted_trades[index].taker_order_id);
        EXPECT_EQ(recovered_trades[index].execution_price, uninterrupted_trades[index].execution_price);
        EXPECT_EQ(recovered_trades[index].execution_quantity, uninterrupted_trades[index].execution_quantity);
    }
    EXPECT_EQ(uninterrupted_trades[0].maker_order_id, OrderId{1});
    EXPECT_EQ(uninterrupted_trades[1].maker_order_id, OrderId{2});
    EXPECT_EQ(uninterrupted_trades[2].maker_order_id, OrderId{3});
}

TEST_F(DurableEngineTest, RepairsTruncatedTailWithoutReplayingOrAppendingIt) {
    {
        auto create_result = DurableEngine::create(journal_path_);
        ASSERT_TRUE(std::holds_alternative<DurableEngine>(create_result));
        DurableEngine engine = std::move(std::get<DurableEngine>(create_result));
        ASSERT_TRUE(std::holds_alternative<SubmitResult>(engine.submit(order(1, Side::Buy, 100, 1))));
        ASSERT_TRUE(std::holds_alternative<SubmitResult>(engine.submit(order(2, Side::Sell, 105, 2))));
    }
    const std::vector<std::byte> partial = encoded_record_or_failure(
        JournalSequence{3},
        SubmitLimitOrderCommand{.order = order(3, Side::Buy, 99, 1)});
    append_file_bytes_or_fail(journal_path_, std::span<const std::byte>(partial).first(11));
    const JournalScanResult before_recovery = JournalReader::scan(journal_path_);
    ASSERT_TRUE(std::holds_alternative<JournalScan>(before_recovery));
    ASSERT_TRUE(std::get<JournalScan>(before_recovery).has_truncated_tail);
    const std::uint64_t valid_size = std::get<JournalScan>(before_recovery).valid_size;

    auto recovery_result = DurableEngine::recover(journal_path_);
    ASSERT_TRUE(std::holds_alternative<DurableEngine>(recovery_result));
    DurableEngine recovered = std::move(std::get<DurableEngine>(recovery_result));
    EXPECT_EQ(std::filesystem::file_size(journal_path_), valid_size);
    EXPECT_FALSE(recovered.order_book().find(OrderId{3}).has_value());
    const JournalScanResult after_recovery = JournalReader::scan(journal_path_);
    ASSERT_TRUE(std::holds_alternative<JournalScan>(after_recovery));
    EXPECT_EQ(std::get<JournalScan>(after_recovery).records.size(), std::size_t{2});

    ASSERT_TRUE(std::holds_alternative<SubmitResult>(
        recovered.submit(order(3, Side::Buy, 99, 1))));
    const JournalScanResult after_append = JournalReader::scan(journal_path_);
    ASSERT_TRUE(std::holds_alternative<JournalScan>(after_append));
    EXPECT_EQ(std::get<JournalScan>(after_append).records.back().sequence, JournalSequence{3});
}

TEST_F(DurableEngineTest, RejectsCorruptJournalDuringRecovery) {
    {
        auto create_result = DurableEngine::create(journal_path_);
        ASSERT_TRUE(std::holds_alternative<DurableEngine>(create_result));
        DurableEngine engine = std::move(std::get<DurableEngine>(create_result));
        ASSERT_TRUE(std::holds_alternative<SubmitResult>(engine.submit(order(1, Side::Buy, 100, 1))));
    }
    std::vector<std::byte> bytes = read_all_or_fail(journal_path_);
    bytes[kJournalFileHeaderSize + kJournalFrameHeaderSize] ^= std::byte{1};
    write_all_or_fail(journal_path_, bytes);

    const auto recovery_result = DurableEngine::recover(journal_path_);
    ASSERT_TRUE(std::holds_alternative<DurableEngineError>(recovery_result));
    const DurableEngineError& error = std::get<DurableEngineError>(recovery_result);
    EXPECT_EQ(error.code, DurableEngineErrorCode::JournalFailure);
    ASSERT_TRUE(error.journal_error.has_value());
    EXPECT_EQ(error.journal_error->code, JournalStorageErrorCode::InvalidJournal);
}

TEST_F(DurableEngineTest, SupportsMultipleRecoveryCyclesWithoutJournalDuplication) {
    {
        auto created = DurableEngine::create(journal_path_);
        ASSERT_TRUE(std::holds_alternative<DurableEngine>(created));
        DurableEngine engine = std::move(std::get<DurableEngine>(created));
        ASSERT_TRUE(std::holds_alternative<SubmitResult>(engine.submit(order(1, Side::Buy, 100, 2))));
    }
    auto first_recovery = DurableEngine::recover(journal_path_);
    ASSERT_TRUE(std::holds_alternative<DurableEngine>(first_recovery));
    {
        DurableEngine engine = std::move(std::get<DurableEngine>(first_recovery));
        ASSERT_TRUE(std::holds_alternative<SubmitResult>(engine.submit(order(2, Side::Sell, 100, 1))));
        ASSERT_TRUE(std::holds_alternative<CancelResult>(engine.cancel(OrderId{1})));
    }
    auto second_recovery = DurableEngine::recover(journal_path_);
    ASSERT_TRUE(std::holds_alternative<DurableEngine>(second_recovery));
    {
        DurableEngine engine = std::move(std::get<DurableEngine>(second_recovery));
        ASSERT_TRUE(std::holds_alternative<SubmitResult>(engine.submit(order(3, Side::Sell, 105, 4))));
    }
    auto third_recovery = DurableEngine::recover(journal_path_);
    ASSERT_TRUE(std::holds_alternative<DurableEngine>(third_recovery));
    DurableEngine engine = std::move(std::get<DurableEngine>(third_recovery));
    EXPECT_EQ(engine.order_book().active_order_count(), std::size_t{1});
    ASSERT_TRUE(engine.order_book().find(OrderId{3}).has_value());
    const JournalScanResult scan_result = JournalReader::scan(journal_path_);
    ASSERT_TRUE(std::holds_alternative<JournalScan>(scan_result));
    ASSERT_EQ(std::get<JournalScan>(scan_result).records.size(), std::size_t{4});
    for (std::size_t index = 0; index < std::get<JournalScan>(scan_result).records.size(); ++index) {
        EXPECT_EQ(
            std::get<JournalScan>(scan_result).records[index].sequence,
            JournalSequence{static_cast<std::uint64_t>(index + 1U)});
    }
}

TEST_F(DurableEngineTest, JournalFailurePreventsApplicationAndPoisonsEngine) {
    auto create_result = DurableEngine::create(journal_path_);
    ASSERT_TRUE(std::holds_alternative<DurableEngine>(create_result));
    DurableEngine engine = std::move(std::get<DurableEngine>(create_result));

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

    const auto failed_submit = engine.submit(order(1, Side::Buy, 100, 1));

    EXPECT_EQ(::setrlimit(RLIMIT_FSIZE, &previous_limit), 0) << std::strerror(errno);
    EXPECT_EQ(::sigaction(SIGXFSZ, &previous_action, nullptr), 0) << std::strerror(errno);

    ASSERT_TRUE(std::holds_alternative<DurableEngineError>(failed_submit));
    EXPECT_EQ(std::get<DurableEngineError>(failed_submit).code, DurableEngineErrorCode::JournalFailure);
    EXPECT_EQ(engine.state(), DurableEngineState::Failed);
    EXPECT_EQ(engine.order_book().active_order_count(), std::size_t{0});
    const auto later_cancel = engine.cancel(OrderId{1});
    ASSERT_TRUE(std::holds_alternative<DurableEngineError>(later_cancel));
    EXPECT_EQ(std::get<DurableEngineError>(later_cancel).code, DurableEngineErrorCode::EngineFailed);
}

TEST_F(DurableEngineTest, BatchExecutionMatchesOrderedSingleCommandSemanticsAndRecovery) {
    const std::vector<JournalCommand> commands{
        SubmitLimitOrderCommand{.order = order(1, Side::Sell, 100, 2)},
        SubmitLimitOrderCommand{.order = order(2, Side::Buy, 100, 1)},
        SubmitLimitOrderCommand{.order = order(2, Side::Buy, 90, 1)},
        SubmitLimitOrderCommand{.order = order(3, Side::Buy, 90, 0)},
        CancelOrderCommand{.order_id = OrderId{1}},
        CancelOrderCommand{.order_id = OrderId{999}},
    };
    auto created = DurableEngine::create(journal_path_);
    ASSERT_TRUE(std::holds_alternative<DurableEngine>(created));
    DurableEngine engine = std::move(std::get<DurableEngine>(created));
    const DurableEngine::BatchResult result = engine.execute_batch(commands);
    ASSERT_TRUE(std::holds_alternative<std::vector<DurableEngine::BatchCommandResult>>(result));
    const auto& results = std::get<std::vector<DurableEngine::BatchCommandResult>>(result);
    ASSERT_EQ(results.size(), commands.size());
    EXPECT_TRUE(std::get<SubmitResult>(results[0]).accepted());
    EXPECT_EQ(std::get<SubmitResult>(results[1]).executed_quantity, Quantity{1});
    EXPECT_EQ(std::get<SubmitResult>(results[2]).rejection_reason, RejectionReason::DuplicateOrderId);
    EXPECT_EQ(std::get<SubmitResult>(results[3]).rejection_reason, RejectionReason::InvalidQuantity);
    EXPECT_EQ(std::get<CancelResult>(results[4]).status, CancelStatus::Cancelled);
    EXPECT_EQ(std::get<CancelResult>(results[5]).status, CancelStatus::NotFound);

    auto recovered_result = DurableEngine::recover(journal_path_);
    ASSERT_TRUE(std::holds_alternative<DurableEngine>(recovered_result));
    DurableEngine recovered = std::move(std::get<DurableEngine>(recovered_result));
    expect_book_equal(engine.order_book(), recovered.order_book(), {OrderId{1}, OrderId{2}, OrderId{3}});
    const JournalScanResult scan = JournalReader::scan(journal_path_);
    ASSERT_TRUE(std::holds_alternative<JournalScan>(scan));
    ASSERT_EQ(std::get<JournalScan>(scan).records.size(), commands.size());
    for (std::size_t index = 0; index < commands.size(); ++index) {
        EXPECT_EQ(std::get<JournalScan>(scan).records[index].sequence,
            JournalSequence{static_cast<std::uint64_t>(index + 1U)});
    }
}

TEST_F(DurableEngineTest, WriteAndSyncFailuresNeverApplyTheLiveBatchAndRecoverActualWalPrefix) {
    const std::array<JournalCommand, 3> commands{
        SubmitLimitOrderCommand{.order = order(1, Side::Buy, 101, 1)},
        SubmitLimitOrderCommand{.order = order(2, Side::Buy, 102, 1)},
        SubmitLimitOrderCommand{.order = order(3, Side::Buy, 103, 1)},
    };
    const std::size_t first_frame_size = encoded_record_or_failure(JournalSequence{1}, commands[0]).size();

    const auto verify_failure = [&](const std::filesystem::path& path, bool fail_sync, std::size_t expected_recovered) {
        {
            auto created = DurableEngine::create(path);
            ASSERT_TRUE(std::holds_alternative<DurableEngine>(created));
            DurableEngine engine = std::move(std::get<DurableEngine>(created));
            if (fail_sync) {
                testing::JournalStorageTestAccess::fail_next_append_sync();
            } else {
                testing::JournalStorageTestAccess::fail_append_write_after(first_frame_size / 2U);
            }
            const DurableEngine::BatchResult result = engine.execute_batch(commands);
            ASSERT_TRUE(std::holds_alternative<DurableEngineError>(result));
            EXPECT_EQ(std::get<DurableEngineError>(result).code, DurableEngineErrorCode::JournalFailure);
            EXPECT_EQ(engine.state(), DurableEngineState::Failed);
            EXPECT_EQ(engine.order_book().active_order_count(), 0U);
            const auto later = engine.submit(order(99, Side::Buy, 99, 1));
            ASSERT_TRUE(std::holds_alternative<DurableEngineError>(later));
            EXPECT_EQ(std::get<DurableEngineError>(later).code, DurableEngineErrorCode::EngineFailed);
        }
        auto recovered_result = DurableEngine::recover(path);
        ASSERT_TRUE(std::holds_alternative<DurableEngine>(recovered_result));
        EXPECT_EQ(std::get<DurableEngine>(recovered_result).order_book().active_order_count(), expected_recovered);
        testing::JournalStorageTestAccess::reset_append_io();
    };

    verify_failure(temporary_directory_ / "mid-write.bin", false, 0U);
    verify_failure(temporary_directory_ / "post-write-sync.bin", true, commands.size());
}

TEST_F(DurableEngineTest, PostSyncApplicationFailurePoisonsLiveEngineButRecoveryReplaysEntireBatch) {
    const std::array<JournalCommand, 3> commands{
        SubmitLimitOrderCommand{.order = order(1, Side::Buy, 101, 1)},
        SubmitLimitOrderCommand{.order = order(2, Side::Buy, 102, 1)},
        SubmitLimitOrderCommand{.order = order(3, Side::Sell, 110, 1)},
    };
    OrderBook expected;
    static_cast<void>(expected.submit(order(1, Side::Buy, 101, 1)));
    static_cast<void>(expected.submit(order(2, Side::Buy, 102, 1)));
    static_cast<void>(expected.submit(order(3, Side::Sell, 110, 1)));

    {
        auto created = DurableEngine::create(journal_path_);
        ASSERT_TRUE(std::holds_alternative<DurableEngine>(created));
        DurableEngine engine = std::move(std::get<DurableEngine>(created));
        testing::DurableEngineTestAccess::set_before_batch_apply_hook([](std::size_t index) {
            if (index == 1U) throw std::runtime_error("injected post-sync application failure");
        });
        const DurableEngine::BatchResult result = engine.execute_batch(commands);
        ASSERT_TRUE(std::holds_alternative<DurableEngineError>(result));
        EXPECT_EQ(std::get<DurableEngineError>(result).code, DurableEngineErrorCode::ApplicationFailure);
        EXPECT_EQ(engine.state(), DurableEngineState::Failed);
        ASSERT_TRUE(engine.order_book().find(OrderId{1}).has_value());
        EXPECT_FALSE(engine.order_book().find(OrderId{2}).has_value());
        const auto later_cancel = engine.cancel(OrderId{1});
        ASSERT_TRUE(std::holds_alternative<DurableEngineError>(later_cancel));
        EXPECT_EQ(std::get<DurableEngineError>(later_cancel).code, DurableEngineErrorCode::EngineFailed);
        testing::DurableEngineTestAccess::clear_before_batch_apply_hook();
    }

    const JournalScanResult scan_result = JournalReader::scan(journal_path_);
    ASSERT_TRUE(std::holds_alternative<JournalScan>(scan_result));
    ASSERT_EQ(std::get<JournalScan>(scan_result).records.size(), commands.size());
    auto recovered_result = DurableEngine::recover(journal_path_);
    ASSERT_TRUE(std::holds_alternative<DurableEngine>(recovered_result));
    expect_book_equal(expected, std::get<DurableEngine>(recovered_result).order_book(), {OrderId{1}, OrderId{2}, OrderId{3}});
}

} // namespace
} // namespace matching
