#include "matching/exchange_server.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <optional>
#include <span>
#include <thread>
#include <variant>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
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
        if (server_.has_value()) {
            stop(*server_);
        }
        std::error_code error;
        std::filesystem::remove_all(directory_, error);
        EXPECT_FALSE(error);
    }

    [[nodiscard]] ExchangeServer& start(ExchangeStartupMode mode = ExchangeStartupMode::CreateNew) {
        ExchangeServer::CreateResult created = ExchangeServer::create({}, journal_path_, mode);
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
    ExchangeServer& server = start(ExchangeStartupMode::Recover);
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

} // namespace
} // namespace matching
