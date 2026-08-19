#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <variant>

#include "matching/detail/bounded_queue.hpp"
#include "matching/gateway_types.hpp"

namespace matching {

namespace testing {
class GatewayServerTestAccess;
}

struct ServerConfig {
    std::string bind_address{"127.0.0.1"};
    std::uint16_t port{};
    std::size_t maximum_connections{256};
    std::size_t maximum_input_buffer_bytes{64U * 1024U};
    std::size_t maximum_output_buffer_bytes{64U * 1024U};
};

enum class GatewayServerErrorCode : std::uint8_t {
    InvalidConfiguration,
    InvalidBindAddress,
    SocketCreationFailed,
    SocketOptionFailed,
    NonblockingFailed,
    BindFailed,
    ListenFailed,
    SocketNameFailed,
    WakeupCreationFailed,
};

struct GatewayServerError {
    GatewayServerErrorCode code;
    std::error_code system_error{};
};

namespace testing {

// Invoked on the gateway thread after a request has been admitted. It is a
// narrow Step 3 test seam for exercising ordered outbound frames; production
// callers leave it empty and no trading response is fabricated.
using GatewayResponseHook = std::function<std::optional<WireEnvelope<ServerMessage>>(
    const EngineRequest&)>;

} // namespace testing

// TCP lifecycle and typed request admission only. It intentionally does not
// claim to be an exchange or durability boundary until a future engine worker
// owns DurableEngine.
class GatewayServer {
public:
    GatewayServer(const GatewayServer&) = delete;
    GatewayServer& operator=(const GatewayServer&) = delete;
    GatewayServer(GatewayServer&&) noexcept;
    GatewayServer& operator=(GatewayServer&&) noexcept;
    ~GatewayServer();

    using CreateResult = std::variant<GatewayServer, GatewayServerError>;

    [[nodiscard]] static CreateResult create(
        ServerConfig config,
        InFlightLimiter& in_flight_limiter,
        gateway_detail::BoundedQueue<AdmittedEngineRequest>& request_queue,
        gateway_detail::BoundedQueue<EngineCompletion>* response_queue = nullptr,
        testing::GatewayResponseHook response_hook = {});

    [[nodiscard]] static CreateResult create(
        ServerConfig config,
        InFlightLimiter& in_flight_limiter,
        gateway_detail::BoundedQueue<AdmittedEngineRequest>& request_queue,
        testing::GatewayResponseHook response_hook);

    [[nodiscard]] std::uint16_t local_port() const noexcept;

    // Uses the calling thread as the sole owner of all socket and connection
    // state. It returns after request_stop() or a fatal listener failure.
    void run();

    // Thread-safe and idempotent. The eventfd wakeup is also the future path
    // for waking poll() when engine responses become available.
    void request_stop() noexcept;

    // Thread-safe notification used by the sole engine worker after it has
    // published completions. It does not touch socket state.
    void notify() noexcept;

    // Thread-safe terminal transition for a poisoned DurableEngine. The
    // gateway stops admission and lets the worker drain admitted commands.
    void enter_fail_stop() noexcept;

private:
    friend class testing::GatewayServerTestAccess;
    class Impl;

    explicit GatewayServer(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_;
};

} // namespace matching
