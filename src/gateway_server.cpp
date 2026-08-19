#include "matching/gateway_server.hpp"

#include "connection.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <limits>
#include <memory>
#include <optional>
#include <poll.h>
#include <span>
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
constexpr std::size_t kMaximumPendingRequestsPerConnection = 160;

[[nodiscard]] GatewayServerError make_error(GatewayServerErrorCode code, int error_number) {
    return GatewayServerError{
        .code = code,
        .system_error = std::error_code(error_number, std::generic_category()),
    };
}

void close_descriptor(int& descriptor) noexcept {
    if (descriptor >= 0) {
        (void)::close(descriptor);
        descriptor = -1;
    }
}

[[nodiscard]] std::optional<int> set_nonblocking(int descriptor) noexcept {
    const int flags = ::fcntl(descriptor, F_GETFL);
    if (flags < 0) {
        return errno;
    }
    if (::fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) != 0) {
        return errno;
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<int> set_close_on_exec(int descriptor) noexcept {
    const int flags = ::fcntl(descriptor, F_GETFD);
    if (flags < 0) {
        return errno;
    }
    if (::fcntl(descriptor, F_SETFD, flags | FD_CLOEXEC) != 0) {
        return errno;
    }
    return std::nullopt;
}

} // namespace

class GatewayServer::Impl {
public:
    struct Connection {
        ConnectionId id;
        int descriptor{-1};
        ClientStreamParser parser;
        gateway_detail::ConnectionOutputBuffer output;
        std::deque<WireEnvelope<ClientMessage>> pending_requests;
        bool closing{};

        Connection(ConnectionId id_in, int descriptor_in, std::size_t input_limit)
            : id(id_in), descriptor(descriptor_in), parser(input_limit) {}

        Connection(const Connection&) = delete;
        Connection& operator=(const Connection&) = delete;
        Connection(Connection&&) noexcept = default;
        Connection& operator=(Connection&&) noexcept = default;

        ~Connection() {
            close_descriptor(descriptor);
        }
    };

    Impl(
        ServerConfig config_in,
        int listener_descriptor_in,
        int wakeup_descriptor_in,
        std::uint16_t port_in,
        InFlightLimiter& in_flight_limiter_in,
        gateway_detail::BoundedQueue<AdmittedEngineRequest>& request_queue_in,
        testing::GatewayResponseHook response_hook_in)
        : config(std::move(config_in)),
          listener_descriptor(listener_descriptor_in),
          wakeup_descriptor(wakeup_descriptor_in),
          port(port_in),
          in_flight_limiter(in_flight_limiter_in),
          request_queue(request_queue_in),
          response_hook(std::move(response_hook_in)) {}

    ~Impl() {
        close_all_connections();
        close_descriptor(listener_descriptor);
        close_descriptor(wakeup_descriptor);
    }

    void request_stop() noexcept {
        stop_requested.store(true, std::memory_order_release);
        const std::uint64_t signal = 1;
        while (true) {
            const ssize_t result = ::write(wakeup_descriptor, &signal, sizeof(signal));
            if (result == static_cast<ssize_t>(sizeof(signal))) {
                return;
            }
            if (result < 0 && errno == EINTR) {
                continue;
            }
            // EAGAIN means a prior wakeup is already pending. Other failures
            // cannot make shutdown unsafe because the atomic flag is checked
            // before every poll iteration.
            return;
        }
    }

    void run() {
        while (!stop_requested.load(std::memory_order_acquire)) {
            std::vector<pollfd> poll_descriptors;
            std::vector<int> client_descriptors;
            poll_descriptors.reserve(connections.size() + 2U);
            client_descriptors.reserve(connections.size());
            poll_descriptors.push_back(pollfd{.fd = listener_descriptor, .events = POLLIN, .revents = 0});
            poll_descriptors.push_back(pollfd{.fd = wakeup_descriptor, .events = POLLIN, .revents = 0});
            for (const auto& [descriptor, connection] : connections) {
                short events = 0;
                if (!reads_paused && !connection.closing) {
                    events = static_cast<short>(events | POLLIN);
                }
                if (!connection.output.empty()) {
                    events = static_cast<short>(events | POLLOUT);
                }
                poll_descriptors.push_back(pollfd{.fd = descriptor, .events = events, .revents = 0});
                client_descriptors.push_back(descriptor);
            }

            const int poll_result = ::poll(poll_descriptors.data(), poll_descriptors.size(), -1);
            if (poll_result < 0) {
                if (errno == EINTR) {
                    continue;
                }
                request_stop();
                continue;
            }

            if ((poll_descriptors[1].revents & POLLIN) != 0) {
                drain_wakeup();
                if (stop_requested.load(std::memory_order_acquire)) {
                    break;
                }
                drain_pending_requests();
            }
            if ((poll_descriptors[0].revents & POLLIN) != 0 && !stop_requested.load(std::memory_order_acquire)) {
                accept_connections();
            }
            if ((poll_descriptors[0].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
                request_stop();
            }

            for (std::size_t index = 0; index < client_descriptors.size(); ++index) {
                const int descriptor = client_descriptors[index];
                const short events = poll_descriptors[index + 2U].revents;
                auto connection_it = connections.find(descriptor);
                if (connection_it == connections.end()) {
                    continue;
                }
                if ((events & POLLIN) != 0) {
                    receive_from(connection_it->second);
                }
                connection_it = connections.find(descriptor);
                if (connection_it != connections.end() && (events & POLLOUT) != 0) {
                    flush_output(connection_it->second);
                }
                connection_it = connections.find(descriptor);
                if (connection_it != connections.end() && (events & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
                    connection_it->second.closing = true;
                }
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
    testing::GatewayResponseHook response_hook;
    std::atomic<bool> stop_requested{false};
    std::unordered_map<int, Connection> connections;
    std::uint64_t next_connection_id{1};
    bool reads_paused{};

private:
    void drain_wakeup() noexcept {
        std::uint64_t ignored = 0;
        while (true) {
            const ssize_t result = ::read(wakeup_descriptor, &ignored, sizeof(ignored));
            if (result == static_cast<ssize_t>(sizeof(ignored))) {
                continue;
            }
            if (result < 0 && errno == EINTR) {
                continue;
            }
            return;
        }
    }

    void accept_connections() {
        while (true) {
            const int descriptor = ::accept(listener_descriptor, nullptr, nullptr);
            if (descriptor >= 0) {
                if (connections.size() >= config.maximum_connections || next_connection_id == 0) {
                    int rejected = descriptor;
                    close_descriptor(rejected);
                    continue;
                }
                if (set_nonblocking(descriptor).has_value() || set_close_on_exec(descriptor).has_value()) {
                    int rejected = descriptor;
                    close_descriptor(rejected);
                    continue;
                }
                const ConnectionId id{next_connection_id};
                ++next_connection_id;
                connections.emplace(
                    std::piecewise_construct,
                    std::forward_as_tuple(descriptor),
                    std::forward_as_tuple(id, descriptor, config.maximum_input_buffer_bytes));
                continue;
            }
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return;
            }
            return;
        }
    }

    void receive_from(Connection& connection) {
        std::array<std::byte, kReceiveBufferSize> bytes{};
        while (!connection.closing && !reads_paused) {
            const ssize_t received = ::recv(connection.descriptor, bytes.data(), bytes.size(), 0);
            if (received > 0) {
                ClientParseFeedResult parsed = connection.parser.feed(
                    std::span<const std::byte>{bytes.data(), static_cast<std::size_t>(received)});
                for (std::size_t index = 0; index < parsed.messages.size(); ++index) {
                    if (!admit(connection, parsed.messages[index])) {
                        for (; index < parsed.messages.size(); ++index) {
                            retain_pending(connection, parsed.messages[index]);
                        }
                        reads_paused = true;
                        break;
                    }
                }
                if (parsed.error.has_value()) {
                    connection.closing = true;
                }
                continue;
            }
            if (received == 0) {
                connection.closing = true;
                return;
            }
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return;
            }
            connection.closing = true;
            return;
        }
    }

    [[nodiscard]] bool admit(Connection& connection, const WireEnvelope<ClientMessage>& request) {
        std::optional<InFlightReservation> reservation = in_flight_limiter.try_acquire();
        if (!reservation.has_value()) {
            return false;
        }
        EngineRequest engine_request = to_engine_request(connection.id, request);
        EngineRequest hook_request = engine_request;
        AdmittedEngineRequest admitted{
            .request = std::move(engine_request),
            .completion = std::move(*reservation),
        };
        if (!request_queue.try_push(std::move(admitted))) {
            return false;
        }
        if (response_hook) {
            const std::optional<WireEnvelope<ServerMessage>> response = response_hook(hook_request);
            if (response.has_value()) {
                queue_response(connection, *response);
            }
        }
        return true;
    }

    void retain_pending(Connection& connection, const WireEnvelope<ClientMessage>& request) {
        if (connection.pending_requests.size() == kMaximumPendingRequestsPerConnection) {
            connection.closing = true;
            return;
        }
        connection.pending_requests.push_back(request);
    }

    void drain_pending_requests() {
        for (auto& [ignored, connection] : connections) {
            while (!connection.pending_requests.empty()) {
                if (!admit(connection, connection.pending_requests.front())) {
                    reads_paused = true;
                    return;
                }
                connection.pending_requests.pop_front();
            }
        }
        reads_paused = false;
    }

    void queue_response(Connection& connection, const WireEnvelope<ServerMessage>& response) {
        const EncodedWireFrame encoded = encode_server_frame(response);
        if (const auto* bytes = std::get_if<std::vector<std::byte>>(&encoded)) {
            if (!connection.output.append(*bytes, config.maximum_output_buffer_bytes)) {
                connection.closing = true;
            }
            return;
        }
        connection.closing = true;
    }

    void flush_output(Connection& connection) {
        const gateway_detail::OutputFlushResult result = connection.output.flush(
            [&connection](const std::byte* bytes, std::size_t size) {
                return ::send(connection.descriptor, bytes, size, MSG_NOSIGNAL);
            });
        if (result == gateway_detail::OutputFlushResult::Fatal) {
            connection.closing = true;
        }
    }

    void close_if_finished(int descriptor) {
        const auto connection = connections.find(descriptor);
        if (connection != connections.end() && connection->second.closing && connection->second.output.empty()) {
            connections.erase(connection);
        }
    }

    void close_all_connections() noexcept {
        connections.clear();
    }
};

GatewayServer::GatewayServer(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

GatewayServer::GatewayServer(GatewayServer&&) noexcept = default;
GatewayServer& GatewayServer::operator=(GatewayServer&&) noexcept = default;
GatewayServer::~GatewayServer() = default;

GatewayServer::CreateResult GatewayServer::create(
    ServerConfig config,
    InFlightLimiter& in_flight_limiter,
    gateway_detail::BoundedQueue<AdmittedEngineRequest>& request_queue,
    testing::GatewayResponseHook response_hook) {
    if (config.bind_address.empty() || config.maximum_connections == 0 ||
        config.maximum_input_buffer_bytes < kWireFrameHeaderSize + kCancelOrderRequestPayloadSize ||
        config.maximum_output_buffer_bytes == 0) {
        return make_error(GatewayServerErrorCode::InvalidConfiguration, 0);
    }

    in_addr address{};
    if (::inet_pton(AF_INET, config.bind_address.c_str(), &address) != 1) {
        return make_error(GatewayServerErrorCode::InvalidBindAddress, 0);
    }

    int listener_descriptor = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listener_descriptor < 0) {
        return make_error(GatewayServerErrorCode::SocketCreationFailed, errno);
    }
    const int reuse_address = 1;
    if (::setsockopt(
            listener_descriptor,
            SOL_SOCKET,
            SO_REUSEADDR,
            &reuse_address,
            static_cast<socklen_t>(sizeof(reuse_address))) != 0) {
        const int error_number = errno;
        close_descriptor(listener_descriptor);
        return make_error(GatewayServerErrorCode::SocketOptionFailed, error_number);
    }
    if (const auto error = set_nonblocking(listener_descriptor); error.has_value()) {
        close_descriptor(listener_descriptor);
        return make_error(GatewayServerErrorCode::NonblockingFailed, *error);
    }
    if (const auto error = set_close_on_exec(listener_descriptor); error.has_value()) {
        close_descriptor(listener_descriptor);
        return make_error(GatewayServerErrorCode::NonblockingFailed, *error);
    }

    sockaddr_in socket_address{};
    socket_address.sin_family = AF_INET;
    socket_address.sin_addr = address;
    socket_address.sin_port = htons(config.port);
    if (::bind(
            listener_descriptor,
            reinterpret_cast<const sockaddr*>(&socket_address),
            static_cast<socklen_t>(sizeof(socket_address))) != 0) {
        const int error_number = errno;
        close_descriptor(listener_descriptor);
        return make_error(GatewayServerErrorCode::BindFailed, error_number);
    }
    const int backlog = config.maximum_connections > static_cast<std::size_t>(std::numeric_limits<int>::max())
                            ? std::numeric_limits<int>::max()
                            : static_cast<int>(config.maximum_connections);
    if (::listen(listener_descriptor, backlog) != 0) {
        const int error_number = errno;
        close_descriptor(listener_descriptor);
        return make_error(GatewayServerErrorCode::ListenFailed, error_number);
    }
    sockaddr_in bound_address{};
    socklen_t bound_address_size = static_cast<socklen_t>(sizeof(bound_address));
    if (::getsockname(
            listener_descriptor,
            reinterpret_cast<sockaddr*>(&bound_address),
            &bound_address_size) != 0) {
        const int error_number = errno;
        close_descriptor(listener_descriptor);
        return make_error(GatewayServerErrorCode::SocketNameFailed, error_number);
    }

    const int wakeup_descriptor = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (wakeup_descriptor < 0) {
        const int error_number = errno;
        close_descriptor(listener_descriptor);
        return make_error(GatewayServerErrorCode::WakeupCreationFailed, error_number);
    }

    return GatewayServer{std::make_unique<Impl>(
        std::move(config),
        listener_descriptor,
        wakeup_descriptor,
        ntohs(bound_address.sin_port),
        in_flight_limiter,
        request_queue,
        std::move(response_hook))};
}

std::uint16_t GatewayServer::local_port() const noexcept {
    return impl_ == nullptr ? 0 : impl_->port;
}

void GatewayServer::run() {
    if (impl_ != nullptr) {
        impl_->run();
    }
}

void GatewayServer::request_stop() noexcept {
    if (impl_ != nullptr) {
        impl_->request_stop();
    }
}

} // namespace matching
