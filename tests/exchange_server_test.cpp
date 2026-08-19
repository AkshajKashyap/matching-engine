#include "matching/exchange_server.hpp"

#include "exchange_server_test_access.hpp"
#include "gateway_server_test_access.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <future>
#include <latch>
#include <optional>
#include <span>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <variant>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/resource.h>
#include <unistd.h>

#include <gtest/gtest.h>

namespace matching {
namespace {

using namespace std::chrono_literals;

class ExchangeServerTest : public ::testing::Test {
protected:
    void SetUp() override {
        std::array<char, 64> template_path{};
        constexpr char pattern[] = "/tmp/matching-engine-exchange-XXXXXX";
        std::memcpy(template_path.data(), pattern, sizeof(pattern));
        ASSERT_NE(::mkdtemp(template_path.data()), nullptr);
        directory_ = template_path.data();
        journal_path_ = directory_ / "journal.bin";
    }

    void TearDown() override {
        testing::ExchangeServerTestAccess::clear_before_execute_hook();
        testing::GatewayServerTestAccess::clear_before_response_drain_hook();
        if (server_.has_value()) {
            stop(*server_);
        }
        std::error_code error;
        std::filesystem::remove_all(directory_, error);
        EXPECT_FALSE(error);
    }

    [[nodiscard]] ExchangeServer& start(
        ExchangeServerConfig config = {},
        ExchangeStartupMode mode = ExchangeStartupMode::CreateNew) {
        ExchangeServer::CreateResult created = ExchangeServer::create(std::move(config), journal_path_, mode);
        EXPECT_TRUE(std::holds_alternative<ExchangeServer>(created));
        server_.emplace(std::move(std::get<ExchangeServer>(created)));
        gateway_thread_.emplace([this] { server_->run(); });
        return *server_;
    }

    void stop(ExchangeServer& server) {
        server.request_stop();
        if (gateway_thread_.has_value()) {
            gateway_thread_->join();
            gateway_thread_.reset();
        }
        server_.reset();
    }

    [[nodiscard]] int connect_client(const ExchangeServer& server) {
        const int descriptor = ::socket(AF_INET, SOCK_STREAM, 0);
        EXPECT_GE(descriptor, 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(server.local_port());
        EXPECT_EQ(::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr), 1);
        EXPECT_EQ(::connect(descriptor, reinterpret_cast<const sockaddr*>(&address),
            static_cast<socklen_t>(sizeof(address))), 0);
        return descriptor;
    }

    static void send_all(int descriptor, std::span<const std::byte> bytes) {
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            const ssize_t sent = ::send(descriptor, bytes.data() + offset, bytes.size() - offset, MSG_NOSIGNAL);
            ASSERT_GT(sent, 0);
            offset += static_cast<std::size_t>(sent);
        }
    }

    static WireEnvelope<ServerMessage> receive_response(int descriptor) {
        std::vector<std::byte> bytes;
        const auto deadline = std::chrono::steady_clock::now() + 1s;
        while (std::chrono::steady_clock::now() < deadline) {
            pollfd event{.fd = descriptor, .events = POLLIN, .revents = 0};
            if (::poll(&event, 1, 20) <= 0) continue;
            std::array<std::byte, 128> chunk{};
            const ssize_t count = ::recv(descriptor, chunk.data(), chunk.size(), 0);
            if (count <= 0) break;
            bytes.insert(bytes.end(), chunk.begin(), chunk.begin() + count);
            const DecodedServerFrameResult decoded = decode_server_frame(bytes);
            if (const auto* frame = std::get_if<DecodedServerFrame>(&decoded)) return frame->envelope;
        }
        ADD_FAILURE() << "timed out waiting for server response";
        return {};
    }

    static std::vector<WireEnvelope<ServerMessage>> receive_responses(
        int descriptor, std::size_t expected_count) {
        std::vector<WireEnvelope<ServerMessage>> responses;
        std::vector<std::byte> buffered;
        const auto deadline = std::chrono::steady_clock::now() + 10s;
        while (responses.size() < expected_count && std::chrono::steady_clock::now() < deadline) {
            while (!buffered.empty()) {
                const DecodedServerFrameResult decoded = decode_server_frame_prefix(buffered);
                if (const auto* frame = std::get_if<DecodedServerFrame>(&decoded)) {
                    responses.push_back(frame->envelope);
                    buffered.erase(buffered.begin(), buffered.begin() + static_cast<std::ptrdiff_t>(frame->bytes_consumed));
                    continue;
                }
                if (std::get<WireCodecError>(decoded) == WireCodecError::TruncatedInput) {
                    break;
                }
                ADD_FAILURE() << "malformed server response stream";
                return responses;
            }
            if (responses.size() == expected_count) {
                break;
            }
            pollfd event{.fd = descriptor, .events = POLLIN, .revents = 0};
            if (::poll(&event, 1, 50) <= 0) {
                continue;
            }
            std::array<std::byte, 4096> chunk{};
            const ssize_t count = ::recv(descriptor, chunk.data(), chunk.size(), 0);
            if (count <= 0) {
                break;
            }
            buffered.insert(buffered.end(), chunk.begin(), chunk.begin() + count);
        }
        EXPECT_EQ(responses.size(), expected_count);
        return responses;
    }

    static void send_fragmented(int descriptor, std::span<const std::byte> bytes, std::uint32_t seed) {
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            seed = seed * 1664525U + 1013904223U;
            const std::size_t chunk = std::min<std::size_t>(
                bytes.size() - offset, 1U + static_cast<std::size_t>((seed >> 16U) % 47U));
            send_all(descriptor, bytes.subspan(offset, chunk));
            offset += chunk;
        }
    }

    static void apply_record(OrderBook& book, const JournalRecord& record) {
        std::visit([&book](const auto& command) {
            using Command = std::decay_t<decltype(command)>;
            if constexpr (std::is_same_v<Command, SubmitLimitOrderCommand>) {
                static_cast<void>(book.submit(command.order));
            } else {
                static_cast<void>(book.cancel(command.order_id));
            }
        }, record.command);
    }

    static void expect_same_book(const OrderBook& expected, const OrderBook& actual) {
        EXPECT_EQ(expected.active_order_count(), actual.active_order_count());
        const auto compare_quote = [](const std::optional<Quote>& left, const std::optional<Quote>& right) {
            EXPECT_EQ(left.has_value(), right.has_value());
            if (left.has_value() && right.has_value()) {
                EXPECT_EQ(left->price, right->price);
                EXPECT_EQ(left->quantity, right->quantity);
            }
        };
        compare_quote(expected.best_bid(), actual.best_bid());
        compare_quote(expected.best_ask(), actual.best_ask());
        for (const Side side : {Side::Buy, Side::Sell}) {
            const auto left = expected.depth(side, 4096);
            const auto right = actual.depth(side, 4096);
            ASSERT_EQ(left.size(), right.size());
            for (std::size_t index = 0; index < left.size(); ++index) {
                EXPECT_EQ(left[index].price, right[index].price);
                EXPECT_EQ(left[index].total_quantity, right[index].total_quantity);
                EXPECT_EQ(left[index].order_count, right[index].order_count);
            }
        }
    }

    template <typename Predicate>
    static testing::ExchangeServerRuntimeStats await_stats(
        const ExchangeServer& server, Predicate&& predicate) {
        const auto deadline = std::chrono::steady_clock::now() + 1s;
        testing::ExchangeServerRuntimeStats stats;
        while (std::chrono::steady_clock::now() < deadline) {
            stats = testing::ExchangeServerTestAccess::stats(server);
            if (predicate(stats)) {
                return stats;
            }
            std::this_thread::yield();
        }
        ADD_FAILURE() << "timed out waiting for exchange runtime state";
        return stats;
    }

    [[nodiscard]] static std::vector<std::byte> submit(
        RequestId request_id, OrderId order_id, std::uint8_t side, std::uint64_t price, std::uint64_t quantity) {
        const EncodedWireFrame encoded = encode_client_frame(WireEnvelope<ClientMessage>{
            .request_id = request_id,
            .message = SubmitLimitOrderRequest{.order_id = order_id, .raw_side = side,
                .limit_price = Price{price}, .quantity = Quantity{quantity}},
        });
        EXPECT_TRUE(std::holds_alternative<std::vector<std::byte>>(encoded));
        return std::get<std::vector<std::byte>>(encoded);
    }

    [[nodiscard]] static std::vector<std::byte> cancel(RequestId request_id, OrderId order_id) {
        const EncodedWireFrame encoded = encode_client_frame(WireEnvelope<ClientMessage>{
            .request_id = request_id,
            .message = CancelOrderRequest{.order_id = order_id},
        });
        EXPECT_TRUE(std::holds_alternative<std::vector<std::byte>>(encoded));
        return std::get<std::vector<std::byte>>(encoded);
    }

    std::filesystem::path directory_;
    std::filesystem::path journal_path_;
    std::optional<ExchangeServer> server_;
    std::optional<std::jthread> gateway_thread_;
};

TEST_F(ExchangeServerTest, NetworkSubmitMatchCancelAndSemanticRejectionAreDurable) {
    ExchangeServer& server = start();
    const int client = connect_client(server);

    send_all(client, submit(RequestId{77}, OrderId{1}, static_cast<std::uint8_t>(Side::Sell), 100, 5));
    const auto maker = receive_response(client);
    EXPECT_EQ(maker.request_id, RequestId{77});
    const auto& maker_result = std::get<SubmitResultResponse>(maker.message);
    EXPECT_EQ(maker_result.status, WireSubmitStatus::Accepted);
    EXPECT_EQ(maker_result.executed_quantity, Quantity{0});
    EXPECT_EQ(maker_result.resting_quantity, Quantity{5});

    send_all(client, submit(RequestId{2}, OrderId{2}, static_cast<std::uint8_t>(Side::Buy), 100, 3));
    const auto taker = receive_response(client);
    EXPECT_EQ(taker.request_id, RequestId{2});
    const auto& taker_result = std::get<SubmitResultResponse>(taker.message);
    EXPECT_EQ(taker_result.status, WireSubmitStatus::Accepted);
    EXPECT_EQ(taker_result.executed_quantity, Quantity{3});
    EXPECT_EQ(taker_result.resting_quantity, Quantity{0});

    send_all(client, submit(RequestId{999}, OrderId{3}, 0xA5U, 101, 1));
    const auto rejected = receive_response(client);
    EXPECT_EQ(rejected.request_id, RequestId{999});
    const auto& rejected_result = std::get<SubmitResultResponse>(rejected.message);
    EXPECT_EQ(rejected_result.status, WireSubmitStatus::Rejected);
    EXPECT_EQ(rejected_result.rejection_code, WireRejectionCode::InvalidSide);

    send_all(client, cancel(RequestId{0}, OrderId{1}));
    const auto cancelled = receive_response(client);
    EXPECT_EQ(cancelled.request_id, RequestId{0});
    const auto& cancel_result = std::get<CancelResultResponse>(cancelled.message);
    EXPECT_EQ(cancel_result.status, WireCancelStatus::Cancelled);
    EXPECT_EQ(cancel_result.cancelled_quantity, Quantity{2});
    ::close(client);
    stop(server);

    auto recovered_result = DurableEngine::recover(journal_path_);
    ASSERT_TRUE(std::holds_alternative<DurableEngine>(recovered_result));
    DurableEngine recovered = std::move(std::get<DurableEngine>(recovered_result));
    EXPECT_FALSE(recovered.order_book().find(OrderId{1}).has_value());
    const JournalScanResult scan = JournalReader::scan(journal_path_);
    ASSERT_TRUE(std::holds_alternative<JournalScan>(scan));
    EXPECT_EQ(std::get<JournalScan>(scan).records.size(), 4U);
}

TEST_F(ExchangeServerTest, RecoverModeContinuesRecoveredBookAndJournalSequence) {
    {
        auto created = DurableEngine::create(journal_path_);
        ASSERT_TRUE(std::holds_alternative<DurableEngine>(created));
        DurableEngine engine = std::move(std::get<DurableEngine>(created));
        static_cast<void>(engine.submit(NewLimitOrder{.id = OrderId{10}, .side = Side::Sell,
            .limit_price = Price{100}, .quantity = Quantity{4}}));
    }
    ExchangeServer& server = start({}, ExchangeStartupMode::Recover);
    const int client = connect_client(server);
    send_all(client, submit(RequestId{1}, OrderId{11}, static_cast<std::uint8_t>(Side::Buy), 100, 4));
    const auto response = receive_response(client);
    const auto& result = std::get<SubmitResultResponse>(response.message);
    EXPECT_EQ(result.executed_quantity, Quantity{4});
    ::close(client);
    stop(server);
    const JournalScanResult scan = JournalReader::scan(journal_path_);
    ASSERT_TRUE(std::holds_alternative<JournalScan>(scan));
    ASSERT_EQ(std::get<JournalScan>(scan).records.size(), 2U);
    EXPECT_EQ(std::get<JournalScan>(scan).records.back().sequence, JournalSequence{2});
}

TEST_F(ExchangeServerTest, DisconnectedClientDoesNotCancelAnAlreadyAdmittedCommand) {
    ExchangeServer& server = start();
    const int client = connect_client(server);
    send_all(client, submit(RequestId{42}, OrderId{99}, static_cast<std::uint8_t>(Side::Buy), 100, 7));
    ::close(client);

    bool journaled = false;
    const auto deadline = std::chrono::steady_clock::now() + 1s;
    while (std::chrono::steady_clock::now() < deadline) {
        const JournalScanResult scan = JournalReader::scan(journal_path_);
        if (const auto* journal = std::get_if<JournalScan>(&scan);
            journal != nullptr && journal->records.size() == 1U && server.in_flight() == 0U) {
            journaled = true;
            break;
        }
        std::this_thread::yield();
    }
    EXPECT_TRUE(journaled);
    stop(server);

    auto recovered_result = DurableEngine::recover(journal_path_);
    ASSERT_TRUE(std::holds_alternative<DurableEngine>(recovered_result));
    EXPECT_TRUE(std::get<DurableEngine>(recovered_result).order_book().find(OrderId{99}).has_value());
}

TEST_F(ExchangeServerTest, CorruptRecoveryFailsBeforeServerIsCreated) {
    {
        auto created = DurableEngine::create(journal_path_);
        ASSERT_TRUE(std::holds_alternative<DurableEngine>(created));
        DurableEngine engine = std::move(std::get<DurableEngine>(created));
        static_cast<void>(engine.submit(NewLimitOrder{.id = OrderId{1}, .side = Side::Buy,
            .limit_price = Price{100}, .quantity = Quantity{1}}));
    }
    const int descriptor = ::open(journal_path_.c_str(), O_RDWR | O_CLOEXEC);
    ASSERT_GE(descriptor, 0);
    const std::byte corrupt{0xFF};
    const off_t end = ::lseek(descriptor, 0, SEEK_END);
    ASSERT_GT(end, 1);
    ASSERT_EQ(::pwrite(descriptor, &corrupt, 1, end - 1), 1);
    ASSERT_EQ(::close(descriptor), 0);
    const auto created = ExchangeServer::create({}, journal_path_, ExchangeStartupMode::Recover);
    ASSERT_TRUE(std::holds_alternative<ExchangeServerError>(created));
    EXPECT_EQ(std::get<ExchangeServerError>(created).code, ExchangeServerErrorCode::DurableEngineStartupFailed);
}

TEST_F(ExchangeServerTest, ConcurrentFragmentedClientsMatchTheJournalAuthoritativeOrder) {
    constexpr std::size_t client_count = 4;
    constexpr std::size_t commands_per_client = 256;
    ExchangeServer& server = start();

    struct ExpectedResponse {
        RequestId request_id;
        bool is_submit;
    };
    std::array<int, client_count> clients{};
    std::array<std::vector<std::byte>, client_count> streams;
    std::array<std::vector<ExpectedResponse>, client_count> expected_responses;
    for (std::size_t client = 0; client < client_count; ++client) {
        clients[client] = connect_client(server);
        const std::uint64_t base = 100000U + static_cast<std::uint64_t>(client) * 10000U;
        for (std::size_t index = 0; index < commands_per_client; ++index) {
            const RequestId request_id{static_cast<std::uint64_t>((index * 37U) % commands_per_client)};
            std::vector<std::byte> encoded;
            bool is_submit = true;
            switch (index % 12U) {
            case 0:
                encoded = submit(request_id, OrderId{base + index}, static_cast<std::uint8_t>(Side::Buy), 90, 2);
                break;
            case 1:
                encoded = submit(request_id, OrderId{base + index}, static_cast<std::uint8_t>(Side::Sell), 110, 3);
                break;
            case 2:
                encoded = submit(request_id, OrderId{base + index}, static_cast<std::uint8_t>(Side::Buy), 200, 1);
                break;
            case 3:
                encoded = submit(request_id, OrderId{base + index}, static_cast<std::uint8_t>(Side::Sell), 1, 1);
                break;
            case 4:
                is_submit = false;
                encoded = cancel(request_id, OrderId{base + 9000U + index});
                break;
            case 5:
                encoded = submit(request_id, OrderId{base + index - 5U}, static_cast<std::uint8_t>(Side::Buy), 90, 1);
                break;
            case 6:
                encoded = submit(request_id, OrderId{base + index}, static_cast<std::uint8_t>(Side::Buy), 0, 1);
                break;
            case 7:
                encoded = submit(request_id, OrderId{base + index}, static_cast<std::uint8_t>(Side::Sell), 110, 0);
                break;
            case 8:
                encoded = submit(request_id, OrderId{base + index}, 0xA5U, 100, 1);
                break;
            case 9:
                is_submit = false;
                encoded = cancel(request_id, OrderId{base});
                break;
            default:
                encoded = submit(request_id, OrderId{base + index},
                    index % 2U == 0U ? static_cast<std::uint8_t>(Side::Buy) : static_cast<std::uint8_t>(Side::Sell),
                    index % 2U == 0U ? 95 : 105, 1);
                break;
            }
            streams[client].insert(streams[client].end(), encoded.begin(), encoded.end());
            expected_responses[client].push_back({.request_id = request_id, .is_submit = is_submit});
        }
    }

    std::barrier start_gate(static_cast<std::ptrdiff_t>(client_count + 1U));
    std::array<std::jthread, client_count> senders;
    for (std::size_t client = 0; client < client_count; ++client) {
        senders[client] = std::jthread([&, client] {
            start_gate.arrive_and_wait();
            send_fragmented(clients[client], streams[client], static_cast<std::uint32_t>(client + 1U));
        });
    }
    start_gate.arrive_and_wait();
    for (auto& sender : senders) {
        sender.join();
    }

    for (std::size_t client = 0; client < client_count; ++client) {
        const auto responses = receive_responses(clients[client], commands_per_client);
        ASSERT_EQ(responses.size(), commands_per_client);
        for (std::size_t index = 0; index < responses.size(); ++index) {
            EXPECT_EQ(responses[index].request_id, expected_responses[client][index].request_id);
            EXPECT_EQ(std::holds_alternative<SubmitResultResponse>(responses[index].message),
                expected_responses[client][index].is_submit);
        }
        ::close(clients[client]);
    }
    EXPECT_LE(server.maximum_observed_in_flight(), 256U);
    stop(server);

    const JournalScanResult scan_result = JournalReader::scan(journal_path_);
    ASSERT_TRUE(std::holds_alternative<JournalScan>(scan_result));
    const JournalScan& scan = std::get<JournalScan>(scan_result);
    ASSERT_EQ(scan.records.size(), client_count * commands_per_client);
    OrderBook expected;
    for (const JournalRecord& record : scan.records) {
        apply_record(expected, record);
    }
    auto recovered_result = DurableEngine::recover(journal_path_);
    ASSERT_TRUE(std::holds_alternative<DurableEngine>(recovered_result));
    DurableEngine recovered = std::move(std::get<DurableEngine>(recovered_result));
    expect_same_book(expected, recovered.order_book());

    const NewLimitOrder next{.id = OrderId{9999999}, .side = Side::Buy, .limit_price = Price{1000}, .quantity = Quantity{17}};
    const SubmitResult expected_next = expected.submit(next);
    const auto recovered_next = recovered.submit(next);
    ASSERT_TRUE(std::holds_alternative<SubmitResult>(recovered_next));
    const SubmitResult& actual_next = std::get<SubmitResult>(recovered_next);
    EXPECT_EQ(actual_next.status, expected_next.status);
    EXPECT_EQ(actual_next.rejection_reason, expected_next.rejection_reason);
    EXPECT_EQ(actual_next.executed_quantity, expected_next.executed_quantity);
    EXPECT_EQ(actual_next.resting_quantity, expected_next.resting_quantity);
    ASSERT_EQ(actual_next.trades.size(), expected_next.trades.size());
    for (std::size_t index = 0; index < actual_next.trades.size(); ++index) {
        EXPECT_EQ(actual_next.trades[index].id, expected_next.trades[index].id);
        EXPECT_EQ(actual_next.trades[index].maker_order_id, expected_next.trades[index].maker_order_id);
        EXPECT_EQ(actual_next.trades[index].execution_price, expected_next.trades[index].execution_price);
        EXPECT_EQ(actual_next.trades[index].execution_quantity, expected_next.trades[index].execution_quantity);
    }
}

TEST_F(ExchangeServerTest, GlobalPendingFifoDrainsBeforeNewSocketReadsAtBackpressureCapacity) {
    ExchangeServerConfig config;
    config.maximum_in_flight = 2;
    config.request_queue_capacity = 2;
    std::promise<void> first_execution_started;
    std::shared_future<void> started = first_execution_started.get_future().share();
    std::latch release_first_execution{1};
    std::atomic<bool> block_once{true};
    testing::ExchangeServerTestAccess::set_before_execute_hook([&] {
        if (block_once.exchange(false)) {
            first_execution_started.set_value();
            release_first_execution.wait();
        }
    });
    ExchangeServer& server = start(config);
    const int first = connect_client(server);
    const int second = connect_client(server);

    std::vector<std::byte> first_stream = submit(RequestId{1}, OrderId{1}, static_cast<std::uint8_t>(Side::Buy), 90, 1);
    const auto second_request = submit(RequestId{2}, OrderId{2}, static_cast<std::uint8_t>(Side::Buy), 91, 1);
    const auto third_request = submit(RequestId{3}, OrderId{3}, static_cast<std::uint8_t>(Side::Buy), 92, 1);
    first_stream.insert(first_stream.end(), second_request.begin(), second_request.end());
    first_stream.insert(first_stream.end(), third_request.begin(), third_request.end());
    send_all(first, first_stream);
    ASSERT_EQ(started.wait_for(1s), std::future_status::ready);
    send_all(second, submit(RequestId{4}, OrderId{4}, static_cast<std::uint8_t>(Side::Buy), 93, 1));

    const auto saturated = await_stats(server, [](const auto& stats) { return stats.in_flight == 2U; });
    EXPECT_EQ(saturated.in_flight, 2U);
    EXPECT_LE(saturated.in_flight, 2U);
    EXPECT_LE(saturated.request_queue_size, saturated.request_queue_capacity);
    release_first_execution.count_down();

    const auto first_responses = receive_responses(first, 3);
    const auto second_responses = receive_responses(second, 1);
    ASSERT_EQ(first_responses.size(), 3U);
    ASSERT_EQ(second_responses.size(), 1U);
    ::close(first);
    ::close(second);
    EXPECT_LE(server.maximum_observed_in_flight(), 2U);
    stop(server);
    testing::ExchangeServerTestAccess::clear_before_execute_hook();

    const JournalScanResult scan_result = JournalReader::scan(journal_path_);
    ASSERT_TRUE(std::holds_alternative<JournalScan>(scan_result));
    const auto& records = std::get<JournalScan>(scan_result).records;
    ASSERT_EQ(records.size(), 4U);
    for (std::size_t index = 0; index < records.size(); ++index) {
        const auto& command = std::get<SubmitLimitOrderCommand>(records[index].command);
        EXPECT_EQ(command.order.id, OrderId{static_cast<std::uint64_t>(index + 1U)});
    }
}

TEST_F(ExchangeServerTest, SlowClientOverflowDoesNotPreventHealthyClientProgress) {
    ExchangeServerConfig config;
    config.maximum_in_flight = 4;
    config.request_queue_capacity = 4;
    config.gateway.maximum_output_buffer_bytes = kWireFrameHeaderSize + kSubmitResultResponsePayloadSize;
    std::promise<void> response_drain_blocked;
    const std::shared_future<void> blocked = response_drain_blocked.get_future().share();
    std::latch release_response_drain{1};
    std::atomic<bool> block_once{true};
    testing::GatewayServerTestAccess::set_before_response_drain_hook([&] {
        if (block_once.exchange(false)) {
            response_drain_blocked.set_value();
            release_response_drain.wait();
        }
    });
    ExchangeServer& server = start(config);
    const int slow_client = connect_client(server);
    const int healthy_client = connect_client(server);
    std::vector<std::byte> slow_stream;
    for (std::uint64_t id = 1; id <= 4; ++id) {
        const auto request = submit(RequestId{id}, OrderId{id}, static_cast<std::uint8_t>(Side::Buy), 90, 1);
        slow_stream.insert(slow_stream.end(), request.begin(), request.end());
    }
    send_all(slow_client, slow_stream);
    ASSERT_EQ(blocked.wait_for(1s), std::future_status::ready);
    const auto queued = await_stats(server, [](const auto& stats) { return stats.response_queue_size == 4U; });
    EXPECT_EQ(queued.in_flight, 4U);
    release_response_drain.count_down();

    send_all(healthy_client, submit(RequestId{77}, OrderId{77}, static_cast<std::uint8_t>(Side::Sell), 110, 1));
    const auto healthy_responses = receive_responses(healthy_client, 1);
    ASSERT_EQ(healthy_responses.size(), 1U);
    EXPECT_EQ(healthy_responses.front().request_id, RequestId{77});
    EXPECT_TRUE(std::holds_alternative<SubmitResultResponse>(healthy_responses.front().message));

    pollfd slow_event{.fd = slow_client, .events = POLLIN, .revents = 0};
    ASSERT_GT(::poll(&slow_event, 1, 1000), 0);
    std::byte discarded{};
    EXPECT_EQ(::recv(slow_client, &discarded, 1, 0), 0);
    ::close(slow_client);
    ::close(healthy_client);
    EXPECT_EQ(await_stats(server, [](const auto& stats) { return stats.in_flight == 0U; }).in_flight, 0U);
    stop(server);
    testing::GatewayServerTestAccess::clear_before_response_drain_hook();
}

TEST_F(ExchangeServerTest, PersistenceFailureFailsStopAndCompletesAlreadyAdmittedRequests) {
    std::promise<void> first_execution_started;
    const std::shared_future<void> started = first_execution_started.get_future().share();
    std::latch release_first_execution{1};
    std::atomic<bool> block_once{true};
    testing::ExchangeServerTestAccess::set_before_execute_hook([&] {
        if (block_once.exchange(false)) {
            first_execution_started.set_value();
            release_first_execution.wait();
        }
    });
    ExchangeServer& server = start();
    const int client = connect_client(server);

    struct rlimit previous_limit {};
    ASSERT_EQ(::getrlimit(RLIMIT_FSIZE, &previous_limit), 0);
    struct sigaction previous_action {};
    struct sigaction ignored_action {};
    ignored_action.sa_handler = SIG_IGN;
    ASSERT_EQ(::sigemptyset(&ignored_action.sa_mask), 0);
    ASSERT_EQ(::sigaction(SIGXFSZ, &ignored_action, &previous_action), 0);
    struct rlimit constrained_limit = previous_limit;
    constrained_limit.rlim_cur = static_cast<rlim_t>(kJournalFileHeaderSize);
    ASSERT_EQ(::setrlimit(RLIMIT_FSIZE, &constrained_limit), 0);

    send_all(client, submit(RequestId{1}, OrderId{1}, static_cast<std::uint8_t>(Side::Buy), 90, 1));
    ASSERT_EQ(started.wait_for(1s), std::future_status::ready);
    std::vector<std::byte> queued_requests;
    for (std::uint64_t id = 2; id <= 4; ++id) {
        const auto request = submit(RequestId{id}, OrderId{id}, static_cast<std::uint8_t>(Side::Buy), 90, 1);
        queued_requests.insert(queued_requests.end(), request.begin(), request.end());
    }
    send_all(client, queued_requests);
    EXPECT_EQ(await_stats(server, [](const auto& stats) { return stats.in_flight == 4U; }).in_flight, 4U);
    release_first_execution.count_down();

    const auto responses = receive_responses(client, 4);
    ASSERT_EQ(::setrlimit(RLIMIT_FSIZE, &previous_limit), 0);
    ASSERT_EQ(::sigaction(SIGXFSZ, &previous_action, nullptr), 0);
    ASSERT_EQ(responses.size(), 4U);
    for (const auto& response : responses) {
        EXPECT_TRUE(std::holds_alternative<EngineUnavailableResponse>(response.message));
    }
    ::close(client);
    stop(server);
    testing::ExchangeServerTestAccess::clear_before_execute_hook();

    const JournalScanResult scan_result = JournalReader::scan(journal_path_);
    ASSERT_TRUE(std::holds_alternative<JournalScan>(scan_result));
    EXPECT_TRUE(std::get<JournalScan>(scan_result).records.empty());
    auto recovered = DurableEngine::recover(journal_path_);
    ASSERT_TRUE(std::holds_alternative<DurableEngine>(recovered));
    EXPECT_EQ(std::get<DurableEngine>(recovered).order_book().active_order_count(), 0U);
}

TEST_F(ExchangeServerTest, UnexpectedWorkerExceptionFailsStopAndReleasesQueuedReservations) {
    std::promise<void> first_execution_started;
    const std::shared_future<void> started = first_execution_started.get_future().share();
    std::latch release_first_execution{1};
    std::atomic<bool> throw_once{true};
    testing::ExchangeServerTestAccess::set_before_execute_hook([&] {
        if (throw_once.exchange(false)) {
            first_execution_started.set_value();
            release_first_execution.wait();
            throw std::runtime_error("test worker failure");
        }
    });
    ExchangeServer& server = start();
    const int client = connect_client(server);
    send_all(client, submit(RequestId{1}, OrderId{1}, static_cast<std::uint8_t>(Side::Buy), 90, 1));
    ASSERT_EQ(started.wait_for(1s), std::future_status::ready);
    std::vector<std::byte> queued_requests;
    for (std::uint64_t id = 2; id <= 4; ++id) {
        const auto request = submit(RequestId{id}, OrderId{id}, static_cast<std::uint8_t>(Side::Buy), 90, 1);
        queued_requests.insert(queued_requests.end(), request.begin(), request.end());
    }
    send_all(client, queued_requests);
    EXPECT_EQ(await_stats(server, [](const auto& stats) { return stats.in_flight == 4U; }).in_flight, 4U);
    release_first_execution.count_down();
    const auto responses = receive_responses(client, 4);
    ASSERT_EQ(responses.size(), 4U);
    for (const auto& response : responses) {
        EXPECT_TRUE(std::holds_alternative<EngineUnavailableResponse>(response.message));
    }
    ::close(client);
    stop(server);
    testing::ExchangeServerTestAccess::clear_before_execute_hook();
    const JournalScanResult scan_result = JournalReader::scan(journal_path_);
    ASSERT_TRUE(std::holds_alternative<JournalScan>(scan_result));
    EXPECT_TRUE(std::get<JournalScan>(scan_result).records.empty());
}

TEST_F(ExchangeServerTest, DisconnectBeforeACompleteFrameDoesNotJournalOrReserveWork) {
    ExchangeServer& server = start();
    const int incomplete_client = connect_client(server);
    const auto request = submit(RequestId{1}, OrderId{1}, static_cast<std::uint8_t>(Side::Buy), 90, 1);
    send_all(incomplete_client, std::span{request}.first(request.size() / 2U));
    ::close(incomplete_client);

    const int healthy_client = connect_client(server);
    send_all(healthy_client, submit(RequestId{2}, OrderId{2}, static_cast<std::uint8_t>(Side::Sell), 110, 1));
    ASSERT_EQ(receive_responses(healthy_client, 1).size(), 1U);
    ::close(healthy_client);
    EXPECT_EQ(await_stats(server, [](const auto& stats) { return stats.in_flight == 0U; }).in_flight, 0U);
    stop(server);

    const JournalScanResult scan_result = JournalReader::scan(journal_path_);
    ASSERT_TRUE(std::holds_alternative<JournalScan>(scan_result));
    const auto& records = std::get<JournalScan>(scan_result).records;
    ASSERT_EQ(records.size(), 1U);
    EXPECT_EQ(std::get<SubmitLimitOrderCommand>(records.front().command).order.id, OrderId{2});
}

TEST_F(ExchangeServerTest, GracefulStopDrainsAlreadyAdmittedCommands) {
    std::promise<void> first_execution_started;
    const std::shared_future<void> started = first_execution_started.get_future().share();
    std::latch release_first_execution{1};
    std::atomic<bool> block_once{true};
    testing::ExchangeServerTestAccess::set_before_execute_hook([&] {
        if (block_once.exchange(false)) {
            first_execution_started.set_value();
            release_first_execution.wait();
        }
    });
    ExchangeServer& server = start();
    const int client = connect_client(server);
    std::vector<std::byte> requests;
    for (std::uint64_t id = 1; id <= 4; ++id) {
        const auto request = submit(RequestId{id}, OrderId{id}, static_cast<std::uint8_t>(Side::Buy), 90, 1);
        requests.insert(requests.end(), request.begin(), request.end());
    }
    send_all(client, requests);
    ASSERT_EQ(started.wait_for(1s), std::future_status::ready);
    EXPECT_EQ(await_stats(server, [](const auto& stats) { return stats.in_flight == 4U; }).in_flight, 4U);
    server.request_stop();
    release_first_execution.count_down();
    const auto responses = receive_responses(client, 4);
    ASSERT_EQ(responses.size(), 4U);
    for (const auto& response : responses) {
        EXPECT_TRUE(std::holds_alternative<SubmitResultResponse>(response.message));
    }
    ::close(client);
    if (gateway_thread_.has_value()) {
        gateway_thread_->join();
        gateway_thread_.reset();
    }
    server_.reset();
    testing::ExchangeServerTestAccess::clear_before_execute_hook();

    const JournalScanResult scan_result = JournalReader::scan(journal_path_);
    ASSERT_TRUE(std::holds_alternative<JournalScan>(scan_result));
    EXPECT_EQ(std::get<JournalScan>(scan_result).records.size(), 4U);
}

TEST_F(ExchangeServerTest, ThreeTcpRecoveryCyclesPreserveJournalSequenceAndMatchingState) {
    const std::array<ExchangeStartupMode, 3> modes{
        ExchangeStartupMode::CreateNew, ExchangeStartupMode::Recover, ExchangeStartupMode::Recover};
    for (std::size_t cycle = 0; cycle < modes.size(); ++cycle) {
        ExchangeServer& server = start({}, modes[cycle]);
        const int client = connect_client(server);
        const std::uint64_t id = 100U + static_cast<std::uint64_t>(cycle);
        const std::uint8_t side = cycle % 2U == 0U ? static_cast<std::uint8_t>(Side::Sell)
                                                    : static_cast<std::uint8_t>(Side::Buy);
        send_all(client, submit(RequestId{static_cast<std::uint64_t>(cycle)}, OrderId{id}, side,
            cycle % 2U == 0U ? 100 : 100, 2));
        ASSERT_EQ(receive_responses(client, 1).size(), 1U);
        ::close(client);
        stop(server);
    }
    const JournalScanResult scan_result = JournalReader::scan(journal_path_);
    ASSERT_TRUE(std::holds_alternative<JournalScan>(scan_result));
    const auto& records = std::get<JournalScan>(scan_result).records;
    ASSERT_EQ(records.size(), 3U);
    for (std::size_t index = 0; index < records.size(); ++index) {
        EXPECT_EQ(records[index].sequence, JournalSequence{static_cast<std::uint64_t>(index + 1U)});
    }
    auto recovered = DurableEngine::recover(journal_path_);
    ASSERT_TRUE(std::holds_alternative<DurableEngine>(recovered));
    EXPECT_EQ(std::get<DurableEngine>(recovered).order_book().active_order_count(), 1U);
}

TEST_F(ExchangeServerTest, RepeatedDeadClientsDrainReservationsAndLeaveServerHealthy) {
    constexpr std::size_t disconnected_clients = 64;
    ExchangeServer& server = start();
    for (std::size_t index = 0; index < disconnected_clients; ++index) {
        const int client = connect_client(server);
        const std::uint64_t id = static_cast<std::uint64_t>(index + 1U);
        send_all(client, submit(RequestId{id}, OrderId{id}, static_cast<std::uint8_t>(Side::Buy), 90, 1));
        ::close(client);
        bool drained = false;
        const auto deadline = std::chrono::steady_clock::now() + 1s;
        while (std::chrono::steady_clock::now() < deadline) {
            const JournalScanResult scan_result = JournalReader::scan(journal_path_);
            if (const auto* scan = std::get_if<JournalScan>(&scan_result);
                scan != nullptr && scan->records.size() == index + 1U && server.in_flight() == 0U) {
                drained = true;
                break;
            }
            std::this_thread::yield();
        }
        ASSERT_TRUE(drained) << "disconnect iteration " << index;
    }
    const int healthy_client = connect_client(server);
    send_all(healthy_client, submit(RequestId{1000}, OrderId{1000}, static_cast<std::uint8_t>(Side::Sell), 110, 1));
    ASSERT_EQ(receive_responses(healthy_client, 1).size(), 1U);
    ::close(healthy_client);
    stop(server);
    const JournalScanResult scan_result = JournalReader::scan(journal_path_);
    ASSERT_TRUE(std::holds_alternative<JournalScan>(scan_result));
    EXPECT_EQ(std::get<JournalScan>(scan_result).records.size(), disconnected_clients + 1U);
}

} // namespace
} // namespace matching
