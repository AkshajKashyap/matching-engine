#include "matching/exchange_server.hpp"

#include "exchange_server_test_access.hpp"

#include <exception>
#include <atomic>
#include <functional>
#include <mutex>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace matching {
namespace {

std::mutex test_hook_mutex;
std::function<void()> before_execute_hook;
std::mutex batch_drain_hook_mutex;
std::function<void()> before_batch_drain_hook;
std::atomic<bool> batch_drain_hook_active{false};

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

void run_before_batch_drain_hook() {
    if (!batch_drain_hook_active.load(std::memory_order_acquire)) return;
    std::function<void()> hook;
    {
        std::lock_guard lock(batch_drain_hook_mutex);
        hook = before_batch_drain_hook;
    }
    if (hook) hook();
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
          engine(std::move(engine_in)),
          batch_size_histogram(config.request_queue_capacity + 1U) {}

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
    std::size_t commands_processed{};
    std::size_t batches_processed{};
    std::vector<std::size_t> batch_size_histogram;

private:
    void publish(EngineCompletion completion) noexcept {
        // With response capacity == maximum in-flight, a full queue here is a
        // broken pipeline invariant, not socket backpressure.
        if (!response_queue.try_push(std::move(completion))) std::terminate();
        gateway->notify();
    }

    [[nodiscard]] static JournalCommand to_journal_command(const EngineRequest& request) noexcept {
        return std::visit([](const auto& value) -> JournalCommand {
            using Request = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Request, SubmitEngineRequest>) {
                return SubmitLimitOrderCommand{.order = value.order};
            } else {
                return CancelOrderCommand{.order_id = value.order_id};
            }
        }, request);
    }

    [[nodiscard]] static EngineResponse summarize(
        const EngineRequest& request,
        const DurableEngine::BatchCommandResult& result) noexcept {
        return std::visit([&request](const auto& value) -> EngineResponse {
            using Result = std::decay_t<decltype(value)>;
            return std::visit([&value](const auto& request_value) -> EngineResponse {
                using Request = std::decay_t<decltype(request_value)>;
                if constexpr (std::is_same_v<Result, SubmitResult> && std::is_same_v<Request, SubmitEngineRequest>) {
                    return summarize_submit_result(request_value.connection_id, request_value.request_id, value);
                } else if constexpr (std::is_same_v<Result, CancelResult> && std::is_same_v<Request, CancelEngineRequest>) {
                    return summarize_cancel_result(request_value.connection_id, request_value.request_id, value);
                } else {
                    std::terminate();
                }
            }, request);
        }, result);
    }

    void execute_and_publish_batch(std::vector<AdmittedEngineRequest> batch, bool& failed) noexcept {
        ++batches_processed;
        commands_processed += batch.size();
        ++batch_size_histogram[batch.size()];
        if (failed) {
            for (AdmittedEngineRequest& admitted : batch) {
                publish(EngineCompletion{.response = unavailable_response(admitted.request), .completion = std::move(admitted.completion)});
            }
            return;
        }
        try {
            std::vector<JournalCommand> commands;
            commands.reserve(batch.size());
            for (const AdmittedEngineRequest& admitted : batch) commands.push_back(to_journal_command(admitted.request));
            run_before_execute_hook();
            DurableEngine::BatchResult result = engine.execute_batch(commands);
            if (const auto* results = std::get_if<std::vector<DurableEngine::BatchCommandResult>>(&result)) {
                if (results->size() != batch.size()) std::terminate();
                for (std::size_t index = 0; index < batch.size(); ++index) {
                    publish(EngineCompletion{
                        .response = summarize(batch[index].request, (*results)[index]),
                        .completion = std::move(batch[index].completion),
                    });
                }
                return;
            }
            failed = true;
            gateway->enter_fail_stop();
            for (AdmittedEngineRequest& admitted : batch) {
                publish(EngineCompletion{.response = unavailable_response(admitted.request), .completion = std::move(admitted.completion)});
            }
        } catch (...) {
            failed = true;
            gateway->enter_fail_stop();
            for (AdmittedEngineRequest& admitted : batch) {
                publish(EngineCompletion{.response = unavailable_response(admitted.request), .completion = std::move(admitted.completion)});
            }
        }
    }

    void worker_loop() noexcept {
        bool failed = false;
        try {
            while (std::optional<AdmittedEngineRequest> admitted = request_queue.wait_pop()) {
                std::vector<AdmittedEngineRequest> batch;
                batch.reserve(config.max_durable_batch_size);
                batch.push_back(std::move(*admitted));
                run_before_batch_drain_hook();
                while (batch.size() < config.max_durable_batch_size) {
                    std::optional<AdmittedEngineRequest> next = request_queue.try_pop();
                    if (!next.has_value()) break;
                    batch.push_back(std::move(*next));
                }
                execute_and_publish_batch(std::move(batch), failed);
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
    if (config.maximum_in_flight == 0 || config.request_queue_capacity == 0 ||
        config.max_durable_batch_size == 0) {
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

ExchangeBatchStats ExchangeServerTestAccess::batch_stats(const ExchangeServer& server) {
    if (server.impl_ == nullptr) return {};
    const ExchangeServer::Impl& impl = *server.impl_;
    return ExchangeBatchStats{
        .commands_processed = impl.commands_processed,
        .batches_processed = impl.batches_processed,
        .batch_size_histogram = impl.batch_size_histogram,
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

void ExchangeServerTestAccess::set_before_batch_drain_hook(std::function<void()> hook) {
    std::lock_guard lock(batch_drain_hook_mutex);
    before_batch_drain_hook = std::move(hook);
    batch_drain_hook_active.store(static_cast<bool>(before_batch_drain_hook), std::memory_order_release);
}

void ExchangeServerTestAccess::clear_before_batch_drain_hook() {
    std::lock_guard lock(batch_drain_hook_mutex);
    before_batch_drain_hook = {};
    batch_drain_hook_active.store(false, std::memory_order_release);
}

} // namespace testing

} // namespace matching
