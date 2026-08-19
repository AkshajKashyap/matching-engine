#include "matching/exchange_server.hpp"

#include "exchange_server_test_access.hpp"

#include <exception>
#include <functional>
#include <mutex>
#include <thread>
#include <type_traits>
#include <utility>

namespace matching {
namespace {

std::mutex test_hook_mutex;
std::function<void()> before_execute_hook;

void run_before_execute_hook() {
    std::function<void()> hook;
    {
        std::lock_guard lock(test_hook_mutex);
        hook = before_execute_hook;
    }
    if (hook) {
        hook();
    }
}

[[nodiscard]] EngineResponse unavailable_response(const EngineRequest& request) noexcept {
    return std::visit([](const auto& value) -> EngineResponse {
        return EngineUnavailableEngineResponse{.connection_id = value.connection_id, .request_id = value.request_id};
    }, request);
}

} // namespace

class ExchangeServer::Impl {
public:
    Impl(ExchangeServerConfig config_in, DurableEngine engine_in)
        : config(std::move(config_in)),
          in_flight_limiter(config.maximum_in_flight),
          request_queue(config.request_queue_capacity),
          response_queue(response_queue_capacity_for(config.maximum_in_flight)),
          engine(std::move(engine_in)) {}

    ~Impl() {
        request_queue.close();
        if (gateway.has_value()) gateway->request_stop();
        if (worker.joinable()) worker.join();
    }

    void run() {
        if (worker_started || !gateway.has_value()) return;
        worker_started = true;
        worker = std::jthread([this] { worker_loop(); });
        gateway->run();
        if (worker.joinable()) worker.join();
    }

    void request_stop() noexcept { if (gateway.has_value()) gateway->request_stop(); }

    ExchangeServerConfig config;
    InFlightLimiter in_flight_limiter;
    gateway_detail::BoundedQueue<AdmittedEngineRequest> request_queue;
    gateway_detail::BoundedQueue<EngineCompletion> response_queue;
    // Constructed before the listener becomes usable; accessed only by the
    // worker thread once run() starts.
    DurableEngine engine;
    std::optional<GatewayServer> gateway;
    std::jthread worker;
    bool worker_started{};

private:
    void publish(EngineCompletion completion) noexcept {
        // With response capacity == maximum in-flight, a full queue here is a
        // broken pipeline invariant, not socket backpressure.
        if (!response_queue.try_push(std::move(completion))) std::terminate();
        gateway->notify();
    }

    [[nodiscard]] EngineResponse execute(const EngineRequest& request, bool& failed) noexcept {
        if (failed) return unavailable_response(request);
        try {
            run_before_execute_hook();
            return std::visit([this, &failed](const auto& value) -> EngineResponse {
                using Request = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<Request, SubmitEngineRequest>) {
                    const DurableEngine::SubmitCommandResult result = engine.submit(value.order);
                    if (const auto* submit = std::get_if<SubmitResult>(&result)) {
                        return summarize_submit_result(value.connection_id, value.request_id, *submit);
                    }
                } else {
                    const DurableEngine::CancelCommandResult result = engine.cancel(value.order_id);
                    if (const auto* cancel = std::get_if<CancelResult>(&result)) {
                        return summarize_cancel_result(value.connection_id, value.request_id, *cancel);
                    }
                }
                failed = true;
                gateway->enter_fail_stop();
                return unavailable_response(EngineRequest{value});
            }, request);
        } catch (...) {
            failed = true;
            gateway->enter_fail_stop();
            return unavailable_response(request);
        }
    }

    void worker_loop() noexcept {
        bool failed = false;
        try {
            while (std::optional<AdmittedEngineRequest> admitted = request_queue.wait_pop()) {
                publish(EngineCompletion{
                    .response = execute(admitted->request, failed),
                    .completion = std::move(admitted->completion),
                });
            }
        } catch (...) {
            failed = true;
            gateway->enter_fail_stop();
            while (std::optional<AdmittedEngineRequest> admitted = request_queue.wait_pop()) {
                publish(EngineCompletion{
                    .response = unavailable_response(admitted->request),
                    .completion = std::move(admitted->completion),
                });
            }
        }
        response_queue.close();
        gateway->notify();
    }
};

ExchangeServer::ExchangeServer(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
ExchangeServer::ExchangeServer(ExchangeServer&&) noexcept = default;
ExchangeServer& ExchangeServer::operator=(ExchangeServer&&) noexcept = default;
ExchangeServer::~ExchangeServer() = default;

ExchangeServer::CreateResult ExchangeServer::create(
    ExchangeServerConfig config,
    const std::filesystem::path& journal_path,
    ExchangeStartupMode startup_mode) {
    if (config.maximum_in_flight == 0 || config.request_queue_capacity == 0) {
        return ExchangeServerError{.code = ExchangeServerErrorCode::InvalidConfiguration};
    }
    DurableEngine::CreateResult engine_result = startup_mode == ExchangeStartupMode::CreateNew
        ? DurableEngine::create(journal_path) : DurableEngine::recover(journal_path);
    if (const auto* error = std::get_if<DurableEngineError>(&engine_result)) {
        return ExchangeServerError{.code = ExchangeServerErrorCode::DurableEngineStartupFailed,
            .durable_engine_error = *error};
    }
    auto impl = std::make_unique<Impl>(config, std::move(std::get<DurableEngine>(engine_result)));
    GatewayServer::CreateResult gateway_result = GatewayServer::create(
        impl->config.gateway, impl->in_flight_limiter, impl->request_queue, &impl->response_queue);
    if (const auto* error = std::get_if<GatewayServerError>(&gateway_result)) {
        return ExchangeServerError{.code = ExchangeServerErrorCode::GatewayStartupFailed, .gateway_error = *error};
    }
    impl->gateway.emplace(std::move(std::get<GatewayServer>(gateway_result)));
    return ExchangeServer{std::move(impl)};
}

std::uint16_t ExchangeServer::local_port() const noexcept {
    return impl_ == nullptr || !impl_->gateway.has_value() ? 0 : impl_->gateway->local_port();
}
std::size_t ExchangeServer::in_flight() const noexcept {
    return impl_ == nullptr ? 0 : impl_->in_flight_limiter.in_flight();
}
std::size_t ExchangeServer::maximum_observed_in_flight() const noexcept {
    return impl_ == nullptr ? 0 : impl_->in_flight_limiter.maximum_observed();
}
void ExchangeServer::run() { if (impl_ != nullptr) impl_->run(); }
void ExchangeServer::request_stop() noexcept { if (impl_ != nullptr) impl_->request_stop(); }

namespace testing {

ExchangeServerRuntimeStats ExchangeServerTestAccess::stats(const ExchangeServer& server) noexcept {
    if (server.impl_ == nullptr) {
        return {};
    }
    const ExchangeServer::Impl& impl = *server.impl_;
    return ExchangeServerRuntimeStats{
        .in_flight = impl.in_flight_limiter.in_flight(),
        .maximum_observed_in_flight = impl.in_flight_limiter.maximum_observed(),
        .request_queue_size = impl.request_queue.size(),
        .request_queue_capacity = impl.request_queue.capacity(),
        .response_queue_size = impl.response_queue.size(),
        .response_queue_capacity = impl.response_queue.capacity(),
    };
}

void ExchangeServerTestAccess::set_before_execute_hook(std::function<void()> hook) {
    std::lock_guard lock(test_hook_mutex);
    before_execute_hook = std::move(hook);
}

void ExchangeServerTestAccess::clear_before_execute_hook() {
    std::lock_guard lock(test_hook_mutex);
    before_execute_hook = {};
}

} // namespace testing

} // namespace matching
