#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <system_error>
#include <variant>

#include "matching/durable_engine.hpp"
#include "matching/gateway_server.hpp"

namespace matching {

namespace testing {
class ExchangeServerTestAccess;
}

enum class ExchangeStartupMode : std::uint8_t {
    CreateNew,
    Recover,
};

struct ExchangeServerConfig {
    ServerConfig gateway{};
    std::size_t maximum_in_flight{256};
    std::size_t request_queue_capacity{256};
};

enum class ExchangeServerErrorCode : std::uint8_t {
    InvalidConfiguration,
    DurableEngineStartupFailed,
    GatewayStartupFailed,
};

struct ExchangeServerError {
    ExchangeServerErrorCode code;
    std::optional<DurableEngineError> durable_engine_error{};
    std::optional<GatewayServerError> gateway_error{};
};

// The public runtime boundary. The caller thread runs the TCP gateway while
// exactly one private worker owns and mutates DurableEngine and its OrderBook.
class ExchangeServer {
public:
    ExchangeServer(const ExchangeServer&) = delete;
    ExchangeServer& operator=(const ExchangeServer&) = delete;
    ExchangeServer(ExchangeServer&&) noexcept;
    ExchangeServer& operator=(ExchangeServer&&) noexcept;
    ~ExchangeServer();

    using CreateResult = std::variant<ExchangeServer, ExchangeServerError>;

    [[nodiscard]] static CreateResult create(
        ExchangeServerConfig config,
        const std::filesystem::path& journal_path,
        ExchangeStartupMode startup_mode);

    [[nodiscard]] std::uint16_t local_port() const noexcept;
    [[nodiscard]] std::size_t in_flight() const noexcept;
    [[nodiscard]] std::size_t maximum_observed_in_flight() const noexcept;

    // Starts the one engine worker, then uses the caller as the gateway
    // thread. This method returns only after graceful draining and worker join.
    void run();
    void request_stop() noexcept;

private:
    friend class testing::ExchangeServerTestAccess;
    class Impl;
    explicit ExchangeServer(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_;
};

} // namespace matching
