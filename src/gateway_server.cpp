#include "matching/gateway_server.hpp"

#include "connection.hpp"
#include "gateway_server_test_access.hpp"

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <mutex>
#include <memory>
#include <optional>
#include <poll.h>
#include <span>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

namespace matching {
namespace {
constexpr std::size_t kReceiveBufferSize = 4096;
constexpr std::size_t kMaximumPendingRequests = 4096;

std::mutex response_drain_hook_mutex;
std::function<void()> before_response_drain_hook;

void run_before_response_drain_hook() {
    std::function<void()> hook;
    {
        std::lock_guard lock(response_drain_hook_mutex);
        hook = before_response_drain_hook;
    }
    if (hook) {
        hook();
    }
}

[[nodiscard]] GatewayServerError make_error(GatewayServerErrorCode code, int error_number) {
    return GatewayServerError{.code = code, .system_error = std::error_code(error_number, std::generic_category())};
}

void close_descriptor(int& descriptor) noexcept {
    if (descriptor >= 0) {
        static_cast<void>(::close(descriptor));
        descriptor = -1;
    }
}

[[nodiscard]] std::optional<int> set_nonblocking(int descriptor) noexcept {
    const int flags = ::fcntl(descriptor, F_GETFL);
    if (flags < 0) return errno;
    if (::fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) != 0) return errno;
    return std::nullopt;
}

[[nodiscard]] std::optional<int> set_close_on_exec(int descriptor) noexcept {
    const int flags = ::fcntl(descriptor, F_GETFD);
    if (flags < 0) return errno;
    if (::fcntl(descriptor, F_SETFD, flags | FD_CLOEXEC) != 0) return errno;
    return std::nullopt;
}

[[nodiscard]] std::optional<WireEnvelope<ServerMessage>> to_wire_response(const EngineResponse& response) noexcept {
    return std::visit([](const auto& value) -> std::optional<WireEnvelope<ServerMessage>> {
        using Response = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Response, SubmitEngineResponse>) {
            const WireSubmitStatus status = value.status == SubmissionStatus::Accepted
                                                ? WireSubmitStatus::Accepted : WireSubmitStatus::Rejected;
            WireRejectionCode rejection = WireRejectionCode::None;
            if (value.rejection_reason.has_value()) {
                const auto mapped = to_wire_rejection_code(*value.rejection_reason);
                if (!mapped.has_value()) return std::nullopt;
                rejection = *mapped;
            }
            return WireEnvelope<ServerMessage>{.request_id = value.request_id,
                .message = SubmitResultResponse{.status = status, .rejection_code = rejection,
                    .executed_quantity = value.executed_quantity, .resting_quantity = value.resting_quantity}};
        } else if constexpr (std::is_same_v<Response, CancelEngineResponse>) {
            const auto status = to_wire_cancel_status(value.status);
            if (!status.has_value()) return std::nullopt;
            return WireEnvelope<ServerMessage>{.request_id = value.request_id,
                .message = CancelResultResponse{.status = *status, .cancelled_quantity = value.cancelled_quantity}};
        } else {
            return WireEnvelope<ServerMessage>{.request_id = value.request_id, .message = EngineUnavailableResponse{}};
        }
    }, response);
}

[[nodiscard]] ConnectionId response_connection_id(const EngineResponse& response) noexcept {
    return std::visit([](const auto& value) { return value.connection_id; }, response);
}
} // namespace

class GatewayServer::Impl {
public:
    struct Connection {
        ConnectionId id;
        int descriptor{-1};
        ClientStreamParser parser;
        gateway_detail::ConnectionOutputBuffer output;
        bool closing{};
        Connection(ConnectionId id_in, int descriptor_in, std::size_t input_limit)
            : id(id_in), descriptor(descriptor_in), parser(input_limit) {}
        Connection(const Connection&) = delete;
        Connection& operator=(const Connection&) = delete;
        Connection(Connection&&) noexcept = default;
        Connection& operator=(Connection&&) noexcept = default;
        ~Connection() { close_descriptor(descriptor); }
    };
    struct PendingRequest { ConnectionId connection_id; WireEnvelope<ClientMessage> request; };

    Impl(ServerConfig config_in, int listener_in, int wakeup_in, std::uint16_t port_in,
         InFlightLimiter& limiter_in, gateway_detail::BoundedQueue<AdmittedEngineRequest>& requests_in,
         gateway_detail::BoundedQueue<EngineCompletion>* responses_in, testing::GatewayResponseHook hook_in)
        : config(std::move(config_in)), listener_descriptor(listener_in), wakeup_descriptor(wakeup_in), port(port_in),
          in_flight_limiter(limiter_in), request_queue(requests_in), response_queue(responses_in),
          response_hook(std::move(hook_in)) {}
    ~Impl() { close_all_connections(); close_descriptor(listener_descriptor); close_descriptor(wakeup_descriptor); }

    void request_stop() noexcept { stop_requested.store(true, std::memory_order_release); wake(); }
    void enter_fail_stop() noexcept { engine_failed.store(true, std::memory_order_release); request_stop(); }
    void wake() noexcept {
        const std::uint64_t signal = 1;
        while (true) {
            const ssize_t result = ::write(wakeup_descriptor, &signal, sizeof(signal));
            if (result == static_cast<ssize_t>(sizeof(signal))) return;
            if (result < 0 && errno == EINTR) continue;
            return;
        }
    }

    void run() {
        for (;;) {
            begin_shutdown_if_requested();
            drain_engine_completions();
            drain_pending_requests();
            if (shutdown_complete()) { flush_outputs_once(); break; }

            std::vector<pollfd> descriptors;
            std::vector<int> client_descriptors;
            descriptors.reserve(connections.size() + 2U);
            client_descriptors.reserve(connections.size());
            if (listener_descriptor >= 0) descriptors.push_back({.fd = listener_descriptor, .events = POLLIN, .revents = 0});
            descriptors.push_back({.fd = wakeup_descriptor, .events = POLLIN, .revents = 0});
            for (const auto& [descriptor, connection] : connections) {
                short events = 0;
                if (!shutdown_started && !reads_paused && !connection.closing) events = static_cast<short>(events | POLLIN);
                if (!connection.output.empty()) events = static_cast<short>(events | POLLOUT);
                descriptors.push_back({.fd = descriptor, .events = events, .revents = 0});
                client_descriptors.push_back(descriptor);
            }
            const int poll_result = ::poll(descriptors.data(), descriptors.size(), -1);
            if (poll_result < 0) { if (errno == EINTR) continue; request_stop(); continue; }
            const std::size_t wakeup_index = listener_descriptor >= 0 ? 1U : 0U;
            const std::size_t clients_index = wakeup_index + 1U;
            if ((descriptors[wakeup_index].revents & POLLIN) != 0) {
                drain_wakeup(); begin_shutdown_if_requested(); drain_engine_completions(); drain_pending_requests();
            }
            if (listener_descriptor >= 0) {
                const pollfd listener_events = descriptors[0];
                if ((listener_events.revents & POLLIN) != 0 && !shutdown_started) accept_connections();
                if ((listener_events.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) request_stop();
            }
            for (std::size_t index = 0; index < client_descriptors.size(); ++index) {
                const int descriptor = client_descriptors[index];
                const short events = descriptors[clients_index + index].revents;
                auto it = connections.find(descriptor);
                if (it == connections.end()) continue;
                if ((events & POLLIN) != 0) receive_from(it->second);
                it = connections.find(descriptor);
                if (it != connections.end() && (events & POLLOUT) != 0) flush_output(it->second);
                it = connections.find(descriptor);
                if (it != connections.end() && (events & (POLLERR | POLLHUP | POLLNVAL)) != 0) it->second.closing = true;
                close_if_finished(descriptor);
            }
        }
        close_descriptor(listener_descriptor);
        close_all_connections();
    }

    ServerConfig config;
    int listener_descriptor{-1};
    int wakeup_descriptor{-1};
    std::uint16_t port{};
    InFlightLimiter& in_flight_limiter;
    gateway_detail::BoundedQueue<AdmittedEngineRequest>& request_queue;
    gateway_detail::BoundedQueue<EngineCompletion>* response_queue{};
    testing::GatewayResponseHook response_hook;
    std::atomic<bool> stop_requested{false};
    std::atomic<bool> engine_failed{false};
    std::unordered_map<int, Connection> connections;
    std::deque<PendingRequest> pending_requests;
    std::uint64_t next_connection_id{1};
    bool reads_paused{};
    bool shutdown_started{};

private:
    void begin_shutdown_if_requested() {
        if (!stop_requested.load(std::memory_order_acquire) || shutdown_started) return;
        shutdown_started = true;
        close_descriptor(listener_descriptor);
        pending_requests.clear();
        request_queue.close();
    }
    [[nodiscard]] bool shutdown_complete() const {
        return shutdown_started && (response_queue == nullptr || (response_queue->closed() && response_queue->size() == 0U));
    }
    void drain_wakeup() noexcept {
        std::uint64_t ignored = 0;
        while (true) {
            const ssize_t result = ::read(wakeup_descriptor, &ignored, sizeof(ignored));
            if (result == static_cast<ssize_t>(sizeof(ignored))) continue;
            if (result < 0 && errno == EINTR) continue;
            return;
        }
    }
    void accept_connections() {
        while (true) {
            const int descriptor = ::accept(listener_descriptor, nullptr, nullptr);
            if (descriptor >= 0) {
                if (connections.size() >= config.maximum_connections || next_connection_id == 0 ||
                    set_nonblocking(descriptor).has_value() || set_close_on_exec(descriptor).has_value()) {
                    int rejected = descriptor; close_descriptor(rejected); continue;
                }
                const ConnectionId id{next_connection_id++};
                connections.emplace(std::piecewise_construct, std::forward_as_tuple(descriptor),
                    std::forward_as_tuple(id, descriptor, config.maximum_input_buffer_bytes));
                continue;
            }
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            return;
        }
    }
    void receive_from(Connection& connection) {
        std::array<std::byte, kReceiveBufferSize> bytes{};
        while (!connection.closing && !reads_paused && !shutdown_started) {
            const ssize_t received = ::recv(connection.descriptor, bytes.data(), bytes.size(), 0);
            if (received > 0) {
                ClientParseFeedResult parsed = connection.parser.feed({bytes.data(), static_cast<std::size_t>(received)});
                for (std::size_t index = 0; index < parsed.messages.size(); ++index) {
                    if (!admit(connection, parsed.messages[index])) {
                        for (; index < parsed.messages.size(); ++index) retain_pending(connection.id, parsed.messages[index]);
                        reads_paused = true; break;
                    }
                }
                if (parsed.error.has_value()) connection.closing = true;
                continue;
            }
            if (received == 0) { connection.closing = true; return; }
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            connection.closing = true; return;
        }
    }
    [[nodiscard]] bool admit(Connection& connection, const WireEnvelope<ClientMessage>& request) {
        if (shutdown_started || engine_failed.load(std::memory_order_acquire)) return false;
        std::optional<InFlightReservation> reservation = in_flight_limiter.try_acquire();
        if (!reservation.has_value()) return false;
        EngineRequest engine_request = to_engine_request(connection.id, request);
        EngineRequest hook_request = engine_request;
        AdmittedEngineRequest admitted{.request = std::move(engine_request), .completion = std::move(*reservation)};
        if (!request_queue.try_push(std::move(admitted))) return false;
        if (response_hook) {
            const auto response = response_hook(hook_request);
            if (response.has_value()) queue_response(connection, *response);
        }
        return true;
    }
    void retain_pending(ConnectionId connection_id, const WireEnvelope<ClientMessage>& request) {
        if (pending_requests.size() == kMaximumPendingRequests) {
            if (Connection* connection = find_connection(connection_id); connection != nullptr) connection->closing = true;
            return;
        }
        pending_requests.push_back({.connection_id = connection_id, .request = request});
    }
    void drain_pending_requests() {
        if (shutdown_started || engine_failed.load(std::memory_order_acquire)) return;
        while (!pending_requests.empty()) {
            PendingRequest& pending = pending_requests.front();
            Connection* connection = find_connection(pending.connection_id);
            if (connection == nullptr || connection->closing) { pending_requests.pop_front(); continue; }
            if (!admit(*connection, pending.request)) { reads_paused = true; return; }
            pending_requests.pop_front();
        }
        reads_paused = false;
    }
    void drain_engine_completions() {
        if (response_queue == nullptr) return;
        if (response_queue->size() != 0U) {
            run_before_response_drain_hook();
        }
        while (std::optional<EngineCompletion> completion = response_queue->try_pop()) {
            if (Connection* connection = find_connection(response_connection_id(completion->response));
                connection != nullptr && !connection->closing) {
                const auto response = to_wire_response(completion->response);
                if (response.has_value()) queue_response(*connection, *response);
                else { connection->output.clear(); connection->closing = true; }
            }
            completion->completion.release();
        }
    }
    [[nodiscard]] Connection* find_connection(ConnectionId id) {
        for (auto& [ignored, connection] : connections) if (connection.id == id) return &connection;
        return nullptr;
    }
    void queue_response(Connection& connection, const WireEnvelope<ServerMessage>& response) {
        const EncodedWireFrame encoded = encode_server_frame(response);
        if (const auto* bytes = std::get_if<std::vector<std::byte>>(&encoded);
            bytes != nullptr && connection.output.append(*bytes, config.maximum_output_buffer_bytes)) return;
        connection.output.clear(); connection.closing = true;
    }
    void flush_output(Connection& connection) {
        const auto result = connection.output.flush([&connection](const std::byte* bytes, std::size_t size) {
            return ::send(connection.descriptor, bytes, size, MSG_NOSIGNAL);
        });
        if (result == gateway_detail::OutputFlushResult::Fatal) { connection.closing = true; connection.output.clear(); }
    }
    void flush_outputs_once() {
        for (auto& [ignored, connection] : connections) if (!connection.output.empty()) flush_output(connection);
    }
    void close_if_finished(int descriptor) {
        const auto it = connections.find(descriptor);
        if (it != connections.end() && it->second.closing && it->second.output.empty()) connections.erase(it);
    }
    void close_all_connections() noexcept { connections.clear(); }
};

GatewayServer::GatewayServer(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
GatewayServer::GatewayServer(GatewayServer&&) noexcept = default;
GatewayServer& GatewayServer::operator=(GatewayServer&&) noexcept = default;
GatewayServer::~GatewayServer() = default;

GatewayServer::CreateResult GatewayServer::create(ServerConfig config, InFlightLimiter& limiter,
    gateway_detail::BoundedQueue<AdmittedEngineRequest>& requests,
    gateway_detail::BoundedQueue<EngineCompletion>* responses, testing::GatewayResponseHook hook) {
    if (config.bind_address.empty() || config.maximum_connections == 0 ||
        config.maximum_input_buffer_bytes < kWireFrameHeaderSize + kCancelOrderRequestPayloadSize ||
        config.maximum_output_buffer_bytes == 0) return make_error(GatewayServerErrorCode::InvalidConfiguration, 0);
    in_addr address{};
    if (::inet_pton(AF_INET, config.bind_address.c_str(), &address) != 1) return make_error(GatewayServerErrorCode::InvalidBindAddress, 0);
    int listener = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) return make_error(GatewayServerErrorCode::SocketCreationFailed, errno);
    const int reuse = 1;
    if (::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, static_cast<socklen_t>(sizeof(reuse))) != 0) {
        const int error = errno; close_descriptor(listener); return make_error(GatewayServerErrorCode::SocketOptionFailed, error);
    }
    if (const auto error = set_nonblocking(listener); error.has_value()) { close_descriptor(listener); return make_error(GatewayServerErrorCode::NonblockingFailed, *error); }
    if (const auto error = set_close_on_exec(listener); error.has_value()) { close_descriptor(listener); return make_error(GatewayServerErrorCode::NonblockingFailed, *error); }
    sockaddr_in socket_address{};
    socket_address.sin_family = AF_INET; socket_address.sin_addr = address; socket_address.sin_port = htons(config.port);
    if (::bind(listener, reinterpret_cast<const sockaddr*>(&socket_address), static_cast<socklen_t>(sizeof(socket_address))) != 0) {
        const int error = errno; close_descriptor(listener); return make_error(GatewayServerErrorCode::BindFailed, error);
    }
    const int backlog = config.maximum_connections > static_cast<std::size_t>(std::numeric_limits<int>::max())
        ? std::numeric_limits<int>::max() : static_cast<int>(config.maximum_connections);
    if (::listen(listener, backlog) != 0) { const int error = errno; close_descriptor(listener); return make_error(GatewayServerErrorCode::ListenFailed, error); }
    sockaddr_in bound{}; socklen_t bound_size = static_cast<socklen_t>(sizeof(bound));
    if (::getsockname(listener, reinterpret_cast<sockaddr*>(&bound), &bound_size) != 0) {
        const int error = errno; close_descriptor(listener); return make_error(GatewayServerErrorCode::SocketNameFailed, error);
    }
    const int wakeup = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (wakeup < 0) { const int error = errno; close_descriptor(listener); return make_error(GatewayServerErrorCode::WakeupCreationFailed, error); }
    return GatewayServer{std::make_unique<Impl>(std::move(config), listener, wakeup, ntohs(bound.sin_port), limiter, requests, responses, std::move(hook))};
}

GatewayServer::CreateResult GatewayServer::create(ServerConfig config, InFlightLimiter& limiter,
    gateway_detail::BoundedQueue<AdmittedEngineRequest>& requests, testing::GatewayResponseHook hook) {
    return create(std::move(config), limiter, requests, nullptr, std::move(hook));
}

std::uint16_t GatewayServer::local_port() const noexcept { return impl_ == nullptr ? 0 : impl_->port; }
void GatewayServer::run() { if (impl_ != nullptr) impl_->run(); }
void GatewayServer::request_stop() noexcept { if (impl_ != nullptr) impl_->request_stop(); }
void GatewayServer::notify() noexcept { if (impl_ != nullptr) impl_->wake(); }
void GatewayServer::enter_fail_stop() noexcept { if (impl_ != nullptr) impl_->enter_fail_stop(); }

namespace testing {

void GatewayServerTestAccess::set_before_response_drain_hook(std::function<void()> hook) {
    std::lock_guard lock(response_drain_hook_mutex);
    before_response_drain_hook = std::move(hook);
}

void GatewayServerTestAccess::clear_before_response_drain_hook() {
    std::lock_guard lock(response_drain_hook_mutex);
    before_response_drain_hook = {};
}

} // namespace testing
} // namespace matching
