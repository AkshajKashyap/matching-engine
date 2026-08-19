#include "connection.hpp"
#include "matching/gateway_server.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <future>
#include <optional>
#include <span>
#include <thread>
#include <variant>
#include <vector>

#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <gtest/gtest.h>

namespace matching {
namespace {

using namespace std::chrono_literals;

class GatewayServerTest : public ::testing::Test {
protected:
    gateway_detail::BoundedQueue<AdmittedEngineRequest> request_queue{256};
    InFlightLimiter limiter{256};
    std::optional<GatewayServer> server;
    std::optional<std::jthread> gateway_thread;

    void start(
        ServerConfig config = {},
        testing::GatewayResponseHook response_hook = {}) {
        GatewayServer::CreateResult created = GatewayServer::create(
            std::move(config), limiter, request_queue, std::move(response_hook));
        ASSERT_TRUE(std::holds_alternative<GatewayServer>(created));
        server.emplace(std::move(std::get<GatewayServer>(created)));
        gateway_thread.emplace([this] { server->run(); });
    }

    void stop() {
        if (server.has_value()) {
            server->request_stop();
        }
        if (gateway_thread.has_value()) {
            gateway_thread->join();
            gateway_thread.reset();
        }
        server.reset();
    }

    void TearDown() override {
        stop();
    }

    [[nodiscard]] int connect_client() const {
        const int descriptor = ::socket(AF_INET, SOCK_STREAM, 0);
        if (descriptor < 0) {
            ADD_FAILURE() << "socket failed";
            return -1;
        }
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(server->local_port());
        if (::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) != 1 ||
            ::connect(
                descriptor,
                reinterpret_cast<const sockaddr*>(&address),
                static_cast<socklen_t>(sizeof(address))) != 0) {
            ADD_FAILURE() << "loopback connection failed";
            ::close(descriptor);
            return -1;
        }
        return descriptor;
    }

    [[nodiscard]] std::optional<AdmittedEngineRequest> await_request() {
        const auto deadline = std::chrono::steady_clock::now() + 1s;
        while (std::chrono::steady_clock::now() < deadline) {
            if (std::optional<AdmittedEngineRequest> request = request_queue.try_pop()) {
                return request;
            }
            std::this_thread::yield();
        }
        return std::nullopt;
    }
};

[[nodiscard]] WireEnvelope<ClientMessage> submit(RequestId request_id) {
    return WireEnvelope<ClientMessage>{
        .request_id = request_id,
        .message = SubmitLimitOrderRequest{
            .order_id = OrderId{100 + request_id.value},
            .raw_side = static_cast<std::uint8_t>(Side::Buy),
            .limit_price = Price{101},
            .quantity = Quantity{10},
        },
    };
}

[[nodiscard]] WireEnvelope<ClientMessage> cancel(RequestId request_id) {
    return WireEnvelope<ClientMessage>{
        .request_id = request_id,
        .message = CancelOrderRequest{.order_id = OrderId{100 + request_id.value}},
    };
}

[[nodiscard]] std::vector<std::byte> encode(const WireEnvelope<ClientMessage>& request) {
    const EncodedWireFrame encoded = encode_client_frame(request);
    EXPECT_TRUE(std::holds_alternative<std::vector<std::byte>>(encoded));
    return std::get<std::vector<std::byte>>(encoded);
}

void send_all(int descriptor, std::span<const std::byte> bytes) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const ssize_t sent = ::send(descriptor, bytes.data() + offset, bytes.size() - offset, MSG_NOSIGNAL);
        ASSERT_GT(sent, 0);
        offset += static_cast<std::size_t>(sent);
    }
}

[[nodiscard]] std::vector<std::byte> receive_exact(int descriptor, std::size_t expected_size) {
    std::vector<std::byte> received;
    received.reserve(expected_size);
    const auto deadline = std::chrono::steady_clock::now() + 1s;
    while (received.size() < expected_size && std::chrono::steady_clock::now() < deadline) {
        pollfd descriptor_event{.fd = descriptor, .events = POLLIN, .revents = 0};
        const int poll_result = ::poll(&descriptor_event, 1, 50);
        if (poll_result <= 0) {
            continue;
        }
        std::array<std::byte, 128> bytes{};
        const ssize_t count = ::recv(descriptor, bytes.data(), bytes.size(), 0);
        if (count > 0) {
            received.insert(received.end(), bytes.begin(), bytes.begin() + count);
        } else {
            break;
        }
    }
    return received;
}

TEST_F(GatewayServerTest, BindsLoopbackPortZeroAcceptsClientsAndStopsPromptly) {
    start();
    ASSERT_NE(server->local_port(), 0U);
    const int first = connect_client();
    const int second = connect_client();
    ASSERT_GE(first, 0);
    ASSERT_GE(second, 0);
    ::close(first);
    ::close(second);

    std::promise<void> stopped;
    std::future<void> stopped_future = stopped.get_future();
    std::jthread watcher([&] {
        gateway_thread->join();
        stopped.set_value();
    });
    server->request_stop();
    EXPECT_EQ(stopped_future.wait_for(1s), std::future_status::ready);
    watcher.join();
    gateway_thread.reset();
}

TEST_F(GatewayServerTest, AdmitsFragmentedSubmitAndCancelWithMonotonicConnectionIdentity) {
    start();
    const int descriptor = connect_client();
    const std::vector<std::byte> submit_bytes = encode(submit(RequestId{1}));
    for (const std::byte byte : submit_bytes) {
        send_all(descriptor, std::span<const std::byte>{&byte, 1});
    }
    const std::vector<std::byte> cancel_bytes = encode(cancel(RequestId{2}));
    send_all(descriptor, cancel_bytes);

    std::optional<AdmittedEngineRequest> first = await_request();
    std::optional<AdmittedEngineRequest> second = await_request();
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());
    ASSERT_TRUE(std::holds_alternative<SubmitEngineRequest>(first->request));
    ASSERT_TRUE(std::holds_alternative<CancelEngineRequest>(second->request));
    EXPECT_EQ(std::get<SubmitEngineRequest>(first->request).request_id, RequestId{1});
    EXPECT_EQ(std::get<CancelEngineRequest>(second->request).request_id, RequestId{2});
    EXPECT_EQ(std::get<SubmitEngineRequest>(first->request).connection_id, ConnectionId{1});
    EXPECT_EQ(std::get<CancelEngineRequest>(second->request).connection_id, ConnectionId{1});
    ::close(descriptor);
}

TEST_F(GatewayServerTest, ConcatenatedFramesPreservePerConnectionOrder) {
    start();
    const int descriptor = connect_client();
    std::vector<std::byte> bytes = encode(submit(RequestId{11}));
    const std::vector<std::byte> next = encode(cancel(RequestId{12}));
    const std::vector<std::byte> final = encode(submit(RequestId{13}));
    bytes.insert(bytes.end(), next.begin(), next.end());
    bytes.insert(bytes.end(), final.begin(), final.end());
    send_all(descriptor, bytes);

    for (const std::uint64_t id : {11U, 12U, 13U}) {
        std::optional<AdmittedEngineRequest> request = await_request();
        ASSERT_TRUE(request.has_value());
        std::visit([id](const auto& value) { EXPECT_EQ(value.request_id, RequestId{id}); }, request->request);
        std::visit([](const auto& value) { EXPECT_EQ(value.connection_id, ConnectionId{1}); }, request->request);
    }
    ::close(descriptor);
}

TEST_F(GatewayServerTest, RejectsMalformedStreamsWithoutAdmittingOrResynchronizing) {
    start();
    const int descriptor = connect_client();
    std::vector<std::byte> malformed = encode(submit(RequestId{1}));
    malformed[0] = std::byte{0};
    const std::vector<std::byte> valid = encode(cancel(RequestId{2}));
    malformed.insert(malformed.end(), valid.begin(), valid.end());
    send_all(descriptor, malformed);

    pollfd event{.fd = descriptor, .events = POLLIN, .revents = 0};
    ASSERT_GT(::poll(&event, 1, 1000), 0);
    std::byte byte{};
    EXPECT_EQ(::recv(descriptor, &byte, 1, 0), 0);
    EXPECT_FALSE(await_request().has_value());
    ::close(descriptor);
}

TEST_F(GatewayServerTest, RejectsRepresentativeMalformedHeadersFromRealClients) {
    start();
    const std::vector<std::byte> valid = encode(submit(RequestId{9}));
    std::vector<std::vector<std::byte>> malformed_frames;

    std::vector<std::byte> unsupported_version = valid;
    unsupported_version[5] = std::byte{2};
    malformed_frames.push_back(std::move(unsupported_version));

    std::vector<std::byte> oversized = valid;
    oversized[8] = std::byte{0};
    oversized[9] = std::byte{0};
    oversized[10] = std::byte{0x10};
    oversized[11] = std::byte{1};
    malformed_frames.push_back(std::move(oversized));

    std::vector<std::byte> wrong_length = valid;
    wrong_length[11] = std::byte{0x1A};
    malformed_frames.push_back(std::move(wrong_length));

    const WireEnvelope<ServerMessage> response{
        .request_id = RequestId{9},
        .message = EngineUnavailableResponse{},
    };
    malformed_frames.push_back(std::get<std::vector<std::byte>>(encode_server_frame(response)));

    for (const std::vector<std::byte>& frame : malformed_frames) {
        const int descriptor = connect_client();
        send_all(descriptor, frame);
        pollfd event{.fd = descriptor, .events = POLLIN, .revents = 0};
        ASSERT_GT(::poll(&event, 1, 1000), 0);
        std::byte byte{};
        EXPECT_EQ(::recv(descriptor, &byte, 1, 0), 0);
        EXPECT_FALSE(request_queue.try_pop().has_value());
        ::close(descriptor);
    }
}

TEST_F(GatewayServerTest, IncompleteDisconnectDoesNotAdmitAndServerRemainsHealthy) {
    start();
    const int partial_client = connect_client();
    const std::vector<std::byte> bytes = encode(submit(RequestId{1}));
    send_all(partial_client, std::span{bytes}.first(bytes.size() / 2U));
    ::close(partial_client);
    EXPECT_FALSE(await_request().has_value());

    const int healthy_client = connect_client();
    send_all(healthy_client, encode(cancel(RequestId{2})));
    std::optional<AdmittedEngineRequest> request = await_request();
    ASSERT_TRUE(request.has_value());
    ASSERT_TRUE(std::holds_alternative<CancelEngineRequest>(request->request));
    EXPECT_EQ(std::get<CancelEngineRequest>(request->request).connection_id, ConnectionId{2});
    ::close(healthy_client);
}

TEST_F(GatewayServerTest, EnforcesMaximumConnections) {
    ServerConfig config;
    config.maximum_connections = 1;
    start(config);
    const int first = connect_client();
    const int second = connect_client();
    send_all(first, encode(submit(RequestId{1})));
    send_all(second, encode(submit(RequestId{2})));
    std::optional<AdmittedEngineRequest> request = await_request();
    ASSERT_TRUE(request.has_value());
    EXPECT_EQ(std::get<SubmitEngineRequest>(request->request).request_id, RequestId{1});
    EXPECT_FALSE(await_request().has_value());
    ::close(first);
    ::close(second);
}

TEST_F(GatewayServerTest, ConnectionChurnDoesNotReuseConnectionIds) {
    start();
    constexpr std::uint64_t connection_count = 32;
    for (std::uint64_t index = 1; index <= connection_count; ++index) {
        const int descriptor = connect_client();
        ASSERT_GE(descriptor, 0);
        send_all(descriptor, encode(submit(RequestId{index})));
        std::optional<AdmittedEngineRequest> request = await_request();
        ASSERT_TRUE(request.has_value());
        ASSERT_TRUE(std::holds_alternative<SubmitEngineRequest>(request->request));
        EXPECT_EQ(std::get<SubmitEngineRequest>(request->request).connection_id, ConnectionId{index});
        ::close(descriptor);
    }

    const int final_client = connect_client();
    send_all(final_client, encode(cancel(RequestId{100})));
    std::optional<AdmittedEngineRequest> final_request = await_request();
    ASSERT_TRUE(final_request.has_value());
    ASSERT_TRUE(std::holds_alternative<CancelEngineRequest>(final_request->request));
    EXPECT_EQ(std::get<CancelEngineRequest>(final_request->request).connection_id,
              ConnectionId{connection_count + 1U});
    ::close(final_client);
}

TEST_F(GatewayServerTest, TestHookBuffersOrderedServerResponsesAndPreservesRequestIds) {
    testing::GatewayResponseHook response_hook = [](const EngineRequest& request)
        -> std::optional<WireEnvelope<ServerMessage>> {
        RequestId request_id{};
        std::visit([&request_id](const auto& value) { request_id = value.request_id; }, request);
        if (request_id == RequestId{1}) {
            return WireEnvelope<ServerMessage>{
                .request_id = request_id,
                .message = ProtocolErrorResponse{.code = WireProtocolErrorCode::MalformedPayload},
            };
        }
        return WireEnvelope<ServerMessage>{
            .request_id = request_id,
            .message = EngineUnavailableResponse{},
        };
    };
    start({}, std::move(response_hook));
    const int descriptor = connect_client();
    std::vector<std::byte> input = encode(submit(RequestId{1}));
    const std::vector<std::byte> next = encode(cancel(RequestId{2}));
    input.insert(input.end(), next.begin(), next.end());
    send_all(descriptor, input);

    const WireEnvelope<ServerMessage> first{
        .request_id = RequestId{1},
        .message = ProtocolErrorResponse{.code = WireProtocolErrorCode::MalformedPayload},
    };
    const WireEnvelope<ServerMessage> second{
        .request_id = RequestId{2},
        .message = EngineUnavailableResponse{},
    };
    const std::vector<std::byte> expected_first = std::get<std::vector<std::byte>>(encode_server_frame(first));
    const std::vector<std::byte> expected_second = std::get<std::vector<std::byte>>(encode_server_frame(second));
    std::vector<std::byte> expected = expected_first;
    expected.insert(expected.end(), expected_second.begin(), expected_second.end());
    EXPECT_EQ(receive_exact(descriptor, expected.size()), expected);
    ::close(descriptor);
}

TEST(GatewayOutputBufferTest, HandlesForcedShortWritesEintrAndWouldBlock) {
    gateway_detail::ConnectionOutputBuffer output;
    const std::array<std::byte, 4> bytes{
        std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
    ASSERT_TRUE(output.append(bytes, 8));
    EXPECT_FALSE(output.append(std::span{bytes}.first(1), 4));
    int call_count = 0;
    const auto flush_result = output.flush([&call_count](const std::byte*, std::size_t) -> ssize_t {
        ++call_count;
        if (call_count == 1) {
            return 1;
        }
        if (call_count == 2) {
            errno = EINTR;
            return -1;
        }
        errno = EAGAIN;
        return -1;
    });
    EXPECT_EQ(flush_result, gateway_detail::OutputFlushResult::WouldBlock);
    EXPECT_EQ(output.pending_size(), 3U);
    EXPECT_EQ(output.flush([](const std::byte*, std::size_t size) -> ssize_t {
        return static_cast<ssize_t>(size);
    }), gateway_detail::OutputFlushResult::Drained);
    EXPECT_TRUE(output.empty());
}

} // namespace
} // namespace matching
