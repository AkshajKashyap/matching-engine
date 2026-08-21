#include <algorithm>
#include <array>
#include <barrier>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "matching/durable_engine.hpp"
#include "matching/exchange_server.hpp"
#include "matching/journal_storage.hpp"
#include "matching/wire_protocol.hpp"

#include "exchange_server_test_access.hpp"

namespace matching {
namespace {

using Clock = std::chrono::steady_clock;
using Nanoseconds = std::chrono::nanoseconds;

class TemporaryJournal {
public:
    TemporaryJournal() {
        std::array<char, 64> template_path{};
        constexpr char pattern[] = "/tmp/matching-engine-network-benchmark-XXXXXX";
        std::copy(std::begin(pattern), std::end(pattern), template_path.begin());
        char* directory = ::mkdtemp(template_path.data());
        if (directory == nullptr) {
            std::abort();
        }
        directory_ = directory;
        journal_path_ = directory_ / "journal.bin";
    }

    TemporaryJournal(const TemporaryJournal&) = delete;
    TemporaryJournal& operator=(const TemporaryJournal&) = delete;

    ~TemporaryJournal() {
        std::error_code error;
        std::filesystem::remove_all(directory_, error);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return journal_path_; }

private:
    std::filesystem::path directory_;
    std::filesystem::path journal_path_;
};

[[nodiscard]] NewLimitOrder order(
    std::uint64_t id, Side side, std::uint64_t price, std::uint64_t quantity) {
    return NewLimitOrder{
        .id = OrderId{id}, .side = side, .limit_price = Price{price}, .quantity = Quantity{quantity}};
}

[[nodiscard]] std::vector<std::byte> require_encoded(const EncodedWireFrame& encoded) {
    if (!std::holds_alternative<std::vector<std::byte>>(encoded)) {
        std::abort();
    }
    return std::get<std::vector<std::byte>>(encoded);
}

[[nodiscard]] std::vector<std::byte> submit_frame(
    std::uint64_t request_id, std::uint64_t order_id, Side side, std::uint64_t price, std::uint64_t quantity) {
    return require_encoded(encode_client_frame(WireEnvelope<ClientMessage>{
        .request_id = RequestId{request_id},
        .message = SubmitLimitOrderRequest{
            .order_id = OrderId{order_id},
            .raw_side = static_cast<std::uint8_t>(side),
            .limit_price = Price{price},
            .quantity = Quantity{quantity}},
    }));
}

[[nodiscard]] std::vector<std::byte> cancel_frame(std::uint64_t request_id, std::uint64_t order_id) {
    return require_encoded(encode_client_frame(WireEnvelope<ClientMessage>{
        .request_id = RequestId{request_id}, .message = CancelOrderRequest{.order_id = OrderId{order_id}},
    }));
}

class NetworkClient {
public:
    explicit NetworkClient(std::uint16_t port) {
        descriptor_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (descriptor_ < 0) {
            std::abort();
        }
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        if (::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) != 1 ||
            ::connect(descriptor_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
            std::abort();
        }
    }

    NetworkClient(const NetworkClient&) = delete;
    NetworkClient& operator=(const NetworkClient&) = delete;
    NetworkClient(NetworkClient&&) = delete;
    NetworkClient& operator=(NetworkClient&&) = delete;

    ~NetworkClient() {
        if (descriptor_ >= 0) {
            static_cast<void>(::close(descriptor_));
        }
    }

    [[nodiscard]] Nanoseconds round_trip(const std::vector<std::byte>& request, RequestId expected_request_id) {
        const auto started = Clock::now();
        send_all(request);
        const WireEnvelope<ServerMessage> response = receive_one();
        const auto finished = Clock::now();
        if (response.request_id != expected_request_id) {
            std::abort();
        }
        return std::chrono::duration_cast<Nanoseconds>(finished - started);
    }

private:
    void send_all(std::span<const std::byte> bytes) {
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            const ssize_t sent = ::send(descriptor_, bytes.data() + offset, bytes.size() - offset, MSG_NOSIGNAL);
            if (sent <= 0) {
                std::abort();
            }
            offset += static_cast<std::size_t>(sent);
        }
    }

    [[nodiscard]] WireEnvelope<ServerMessage> receive_one() {
        constexpr auto timeout = std::chrono::seconds{30};
        const auto deadline = Clock::now() + timeout;
        while (Clock::now() < deadline) {
            if (!buffer_.empty()) {
                const DecodedServerFrameResult decoded = decode_server_frame_prefix(buffer_);
                if (const auto* frame = std::get_if<DecodedServerFrame>(&decoded)) {
                    WireEnvelope<ServerMessage> response = frame->envelope;
                    buffer_.erase(
                        buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(frame->bytes_consumed));
                    return response;
                }
                if (std::get<WireCodecError>(decoded) != WireCodecError::TruncatedInput) {
                    std::abort();
                }
            }
            pollfd event{.fd = descriptor_, .events = POLLIN, .revents = 0};
            if (::poll(&event, 1, 100) <= 0) {
                continue;
            }
            std::array<std::byte, 4096> bytes{};
            const ssize_t received = ::recv(descriptor_, bytes.data(), bytes.size(), 0);
            if (received <= 0) {
                std::abort();
            }
            buffer_.insert(buffer_.end(), bytes.begin(), bytes.begin() + received);
        }
        std::abort();
    }

    int descriptor_{-1};
    std::vector<std::byte> buffer_;
};

class ExchangeHarness {
public:
    explicit ExchangeHarness(std::size_t max_durable_batch_size = 1) {
        ExchangeServerConfig config;
        config.max_durable_batch_size = max_durable_batch_size;
        ExchangeServer::CreateResult created = ExchangeServer::create(
            config, journal_.path(), ExchangeStartupMode::CreateNew);
        if (!std::holds_alternative<ExchangeServer>(created)) {
            std::abort();
        }
        server_.emplace(std::move(std::get<ExchangeServer>(created)));
        gateway_thread_.emplace([this] { server_->run(); });
    }

    ExchangeHarness(const ExchangeHarness&) = delete;
    ExchangeHarness& operator=(const ExchangeHarness&) = delete;

    ~ExchangeHarness() { static_cast<void>(stop_and_verify(0, false)); }

    [[nodiscard]] NetworkClient connect_client() const { return NetworkClient{server_->local_port()}; }

    [[nodiscard]] testing::ExchangeBatchStats stop_and_verify(
        std::size_t expected_records, bool verify_records = true) {
        if (!server_.has_value()) {
            return {};
        }
        server_->request_stop();
        gateway_thread_->join();
        gateway_thread_.reset();
        const testing::ExchangeBatchStats stats = testing::ExchangeServerTestAccess::batch_stats(*server_);
        server_.reset();
        if (!verify_records) {
            return stats;
        }
        const JournalScanResult scan_result = JournalReader::scan(journal_.path());
        if (!std::holds_alternative<JournalScan>(scan_result) ||
            std::get<JournalScan>(scan_result).records.size() != expected_records) {
            std::abort();
        }
        const auto recovered = DurableEngine::recover(journal_.path());
        if (!std::holds_alternative<DurableEngine>(recovered)) {
            std::abort();
        }
        return stats;
    }

private:
    TemporaryJournal journal_;
    std::optional<ExchangeServer> server_;
    std::optional<std::jthread> gateway_thread_;
};

struct Summary {
    std::size_t samples{};
    double p50_us{};
    double p95_us{};
    double p99_us{};
    double operations_per_second{};
};

struct ThroughputSummary {
    Summary summary;
    testing::ExchangeBatchStats batch_stats;
};

[[nodiscard]] Summary summarize(std::vector<Nanoseconds> samples, Nanoseconds elapsed) {
    if (samples.empty() || elapsed.count() <= 0) {
        std::abort();
    }
    std::sort(samples.begin(), samples.end());
    const auto percentile = [&samples](double fraction) {
        const double position = fraction * static_cast<double>(samples.size() - 1U);
        const std::size_t index = static_cast<std::size_t>(std::ceil(position));
        return static_cast<double>(samples[index].count()) / 1'000.0;
    };
    return Summary{
        .samples = samples.size(),
        .p50_us = percentile(0.50),
        .p95_us = percentile(0.95),
        .p99_us = percentile(0.99),
        .operations_per_second = static_cast<double>(samples.size()) /
            (static_cast<double>(elapsed.count()) / 1'000'000'000.0),
    };
}

[[nodiscard]] Nanoseconds total_duration(const std::vector<Nanoseconds>& samples) {
    Nanoseconds total{};
    for (const Nanoseconds sample : samples) {
        total += sample;
    }
    return total;
}

void print_summary(std::string_view name, const Summary& summary) {
    std::cout << name << ',' << summary.samples << ',' << summary.p50_us << ',' << summary.p95_us << ','
              << summary.p99_us << ',' << summary.operations_per_second << '\n';
}

template <typename Operation>
[[nodiscard]] Summary measure_direct(std::size_t samples, Operation&& operation) {
    std::vector<Nanoseconds> measurements;
    measurements.reserve(samples);
    for (std::size_t index = 0; index < samples; ++index) {
        const auto operation_started = Clock::now();
        operation(index);
        measurements.push_back(std::chrono::duration_cast<Nanoseconds>(Clock::now() - operation_started));
    }
    return summarize(measurements, total_duration(measurements));
}

template <typename Operation>
[[nodiscard]] Summary measure_tcp(std::size_t samples, Operation&& operation, std::size_t journal_records) {
    ExchangeHarness exchange;
    NetworkClient client = exchange.connect_client();
    // Warm the client/server scheduling and socket path. These durable commands
    // are deliberately excluded from the reported distribution.
    for (std::size_t index = 0; index < 10; ++index) {
        operation(client, index, false);
    }
    std::vector<Nanoseconds> measurements;
    measurements.reserve(samples);
    for (std::size_t index = 0; index < samples; ++index) {
        measurements.push_back(operation(client, index, true));
    }
    static_cast<void>(exchange.stop_and_verify(journal_records + 10U * (journal_records / samples)));
    return summarize(measurements, total_duration(measurements));
}

[[nodiscard]] Summary direct_passive(std::size_t samples) {
    TemporaryJournal journal;
    auto created = DurableEngine::create(journal.path());
    if (!std::holds_alternative<DurableEngine>(created)) std::abort();
    DurableEngine engine = std::move(std::get<DurableEngine>(created));
    for (std::size_t index = 0; index < 10; ++index) {
        static_cast<void>(engine.submit(order(index + 1U, Side::Buy, 90, 1)));
    }
    return measure_direct(samples, [&engine](std::size_t index) {
        const auto result = engine.submit(order(100 + index, Side::Buy, 90, 1));
        if (!std::holds_alternative<SubmitResult>(result) || !std::get<SubmitResult>(result).accepted()) std::abort();
    });
}

[[nodiscard]] Summary direct_aggressive(std::size_t samples) {
    TemporaryJournal journal;
    auto created = DurableEngine::create(journal.path());
    if (!std::holds_alternative<DurableEngine>(created)) std::abort();
    DurableEngine engine = std::move(std::get<DurableEngine>(created));
    for (std::size_t index = 0; index < 10; ++index) {
        const std::uint64_t maker = 5'000U + index * 2U;
        static_cast<void>(engine.submit(order(maker, Side::Sell, 100, 1)));
        static_cast<void>(engine.submit(order(maker + 1U, Side::Buy, 100, 1)));
    }
    std::vector<Nanoseconds> measurements;
    measurements.reserve(samples);
    for (std::size_t index = 0; index < samples; ++index) {
        const std::uint64_t maker = 10'000U + index * 2U;
        if (!std::holds_alternative<SubmitResult>(engine.submit(order(maker, Side::Sell, 100, 1)))) std::abort();
        const auto started = Clock::now();
        const auto result = engine.submit(order(maker + 1U, Side::Buy, 100, 1));
        measurements.push_back(std::chrono::duration_cast<Nanoseconds>(Clock::now() - started));
        if (!std::holds_alternative<SubmitResult>(result) || std::get<SubmitResult>(result).executed_quantity != Quantity{1}) std::abort();
    }
    return summarize(measurements, total_duration(measurements));
}

[[nodiscard]] Summary direct_cancel(std::size_t samples) {
    TemporaryJournal journal;
    auto created = DurableEngine::create(journal.path());
    if (!std::holds_alternative<DurableEngine>(created)) std::abort();
    DurableEngine engine = std::move(std::get<DurableEngine>(created));
    for (std::size_t index = 0; index < 10; ++index) {
        const std::uint64_t id = 15'000U + index;
        static_cast<void>(engine.submit(order(id, Side::Buy, 90, 1)));
        static_cast<void>(engine.cancel(OrderId{id}));
    }
    std::vector<Nanoseconds> measurements;
    measurements.reserve(samples);
    for (std::size_t index = 0; index < samples; ++index) {
        const std::uint64_t id = 20'000U + index;
        static_cast<void>(engine.submit(order(id, Side::Buy, 90, 1)));
        const auto started = Clock::now();
        const auto result = engine.cancel(OrderId{id});
        measurements.push_back(std::chrono::duration_cast<Nanoseconds>(Clock::now() - started));
        if (!std::holds_alternative<CancelResult>(result) || std::get<CancelResult>(result).status != CancelStatus::Cancelled) std::abort();
    }
    return summarize(measurements, total_duration(measurements));
}

[[nodiscard]] Summary direct_rejected(std::size_t samples) {
    TemporaryJournal journal;
    auto created = DurableEngine::create(journal.path());
    if (!std::holds_alternative<DurableEngine>(created)) std::abort();
    DurableEngine engine = std::move(std::get<DurableEngine>(created));
    for (std::size_t index = 0; index < 10; ++index) {
        static_cast<void>(engine.submit(order(25'000U + index, Side::Buy, 90, 0)));
    }
    return measure_direct(samples, [&engine](std::size_t index) {
        const auto result = engine.submit(order(30'000U + index, Side::Buy, 90, 0));
        if (!std::holds_alternative<SubmitResult>(result) || std::get<SubmitResult>(result).accepted()) std::abort();
    });
}

[[nodiscard]] Summary direct_mixed(std::size_t samples) {
    TemporaryJournal journal;
    auto created = DurableEngine::create(journal.path());
    if (!std::holds_alternative<DurableEngine>(created)) std::abort();
    DurableEngine engine = std::move(std::get<DurableEngine>(created));
    for (std::size_t index = 0; index < 10; ++index) {
        const std::uint64_t id = 35'000U + index * 4U;
        static_cast<void>(engine.submit(order(id, Side::Sell, 100, 2)));
        static_cast<void>(engine.submit(order(id + 1U, Side::Buy, 100, 1)));
        static_cast<void>(engine.submit(order(id + 2U, Side::Buy, 90, 1)));
        static_cast<void>(engine.cancel(OrderId{id + 2U}));
    }
    std::vector<Nanoseconds> measurements;
    measurements.reserve(samples * 4U);
    for (std::size_t index = 0; index < samples; ++index) {
        const std::uint64_t id = 40'000U + index * 4U;
        const auto measure = [&measurements](auto&& operation) {
            const auto started = Clock::now();
            operation();
            measurements.push_back(std::chrono::duration_cast<Nanoseconds>(Clock::now() - started));
        };
        measure([&] { if (!std::holds_alternative<SubmitResult>(engine.submit(order(id, Side::Sell, 100, 2)))) std::abort(); });
        measure([&] { if (!std::holds_alternative<SubmitResult>(engine.submit(order(id + 1U, Side::Buy, 100, 1)))) std::abort(); });
        measure([&] { if (!std::holds_alternative<SubmitResult>(engine.submit(order(id + 2U, Side::Buy, 90, 1)))) std::abort(); });
        measure([&] { if (!std::holds_alternative<CancelResult>(engine.cancel(OrderId{id + 2U}))) std::abort(); });
    }
    return summarize(measurements, total_duration(measurements));
}

[[nodiscard]] Summary tcp_passive(std::size_t samples) {
    return measure_tcp(samples,
        [](NetworkClient& client, std::size_t index, bool measure) -> Nanoseconds {
            const std::uint64_t id = (measure ? 100U : 1U) + index;
            const auto frame = submit_frame(id, id, Side::Buy, 90, 1);
            return client.round_trip(frame, RequestId{id});
        }, samples);
}

[[nodiscard]] Summary tcp_aggressive(std::size_t samples) {
    return measure_tcp(samples,
        [](NetworkClient& client, std::size_t index, bool measure) -> Nanoseconds {
            const std::uint64_t maker = (measure ? 10'000U : 1'000U) + index * 2U;
            const auto maker_frame = submit_frame(maker, maker, Side::Sell, 100, 1);
            static_cast<void>(client.round_trip(maker_frame, RequestId{maker}));
            const auto taker_frame = submit_frame(maker + 1U, maker + 1U, Side::Buy, 100, 1);
            return client.round_trip(taker_frame, RequestId{maker + 1U});
        }, samples * 2U);
}

[[nodiscard]] Summary tcp_cancel(std::size_t samples) {
    return measure_tcp(samples,
        [](NetworkClient& client, std::size_t index, bool measure) -> Nanoseconds {
            const std::uint64_t id = (measure ? 20'000U : 2'000U) + index;
            static_cast<void>(client.round_trip(submit_frame(id, id, Side::Buy, 90, 1), RequestId{id}));
            return client.round_trip(cancel_frame(id + 50'000U, id), RequestId{id + 50'000U});
        }, samples * 2U);
}

[[nodiscard]] Summary tcp_rejected(std::size_t samples) {
    return measure_tcp(samples,
        [](NetworkClient& client, std::size_t index, bool measure) -> Nanoseconds {
            const std::uint64_t id = (measure ? 30'000U : 3'000U) + index;
            return client.round_trip(submit_frame(id, id, Side::Buy, 90, 0), RequestId{id});
        }, samples);
}

[[nodiscard]] Summary tcp_mixed(std::size_t samples) {
    ExchangeHarness exchange;
    NetworkClient client = exchange.connect_client();
    std::vector<Nanoseconds> measurements;
    measurements.reserve(samples * 4U);
    for (std::size_t index = 0; index < 10; ++index) {
        const std::uint64_t id = 35'000U + index * 4U;
        static_cast<void>(client.round_trip(submit_frame(id, id, Side::Sell, 100, 2), RequestId{id}));
        static_cast<void>(client.round_trip(submit_frame(id + 1U, id + 1U, Side::Buy, 100, 1), RequestId{id + 1U}));
        static_cast<void>(client.round_trip(submit_frame(id + 2U, id + 2U, Side::Buy, 90, 1), RequestId{id + 2U}));
        static_cast<void>(client.round_trip(cancel_frame(id + 3U, id + 2U), RequestId{id + 3U}));
    }
    for (std::size_t index = 0; index < samples; ++index) {
        const std::uint64_t id = 40'000U + index * 4U;
        measurements.push_back(client.round_trip(submit_frame(id, id, Side::Sell, 100, 2), RequestId{id}));
        measurements.push_back(client.round_trip(submit_frame(id + 1U, id + 1U, Side::Buy, 100, 1), RequestId{id + 1U}));
        measurements.push_back(client.round_trip(submit_frame(id + 2U, id + 2U, Side::Buy, 90, 1), RequestId{id + 2U}));
        measurements.push_back(client.round_trip(cancel_frame(id + 3U, id + 2U), RequestId{id + 3U}));
    }
    static_cast<void>(exchange.stop_and_verify((samples + 10U) * 4U));
    return summarize(measurements, total_duration(measurements));
}

[[nodiscard]] ThroughputSummary tcp_passive_throughput(
    std::size_t client_count, std::size_t operations_per_client, std::size_t max_durable_batch_size) {
    ExchangeHarness exchange{max_durable_batch_size};
    std::barrier ready(static_cast<std::ptrdiff_t>(client_count + 1U));
    std::vector<std::vector<Nanoseconds>> measurements(client_count);
    std::vector<std::jthread> clients;
    clients.reserve(client_count);
    for (std::size_t client_index = 0; client_index < client_count; ++client_index) {
        clients.emplace_back([&, client_index] {
            NetworkClient client = exchange.connect_client();
            ready.arrive_and_wait();
            for (std::size_t operation = 0; operation < operations_per_client; ++operation) {
                const std::uint64_t id = 100'000U + static_cast<std::uint64_t>(client_index) * 10'000U + operation;
                measurements[client_index].push_back(
                    client.round_trip(submit_frame(operation, id, Side::Buy, 90, 1), RequestId{operation}));
            }
        });
    }
    ready.arrive_and_wait();
    const auto started = Clock::now();
    for (auto& client : clients) client.join();
    const auto elapsed = std::chrono::duration_cast<Nanoseconds>(Clock::now() - started);
    std::vector<Nanoseconds> flattened;
    for (auto& client_measurements : measurements) {
        flattened.insert(flattened.end(), client_measurements.begin(), client_measurements.end());
    }
    const auto batch_stats = exchange.stop_and_verify(client_count * operations_per_client);
    return ThroughputSummary{.summary = summarize(std::move(flattened), elapsed), .batch_stats = batch_stats};
}

[[nodiscard]] ThroughputSummary tcp_mixed_throughput(
    std::size_t client_count, std::size_t operations_per_client, std::size_t max_durable_batch_size) {
    if (operations_per_client % 4U != 0U) std::abort();
    const std::size_t blocks_per_client = operations_per_client / 4U;
    ExchangeHarness exchange{max_durable_batch_size};
    std::barrier ready(static_cast<std::ptrdiff_t>(client_count + 1U));
    std::vector<std::vector<Nanoseconds>> measurements(client_count);
    std::vector<std::jthread> clients;
    clients.reserve(client_count);
    for (std::size_t client_index = 0; client_index < client_count; ++client_index) {
        clients.emplace_back([&, client_index] {
            NetworkClient client = exchange.connect_client();
            ready.arrive_and_wait();
            const std::uint64_t client_base = 200'000U + static_cast<std::uint64_t>(client_index) * 100'000U;
            for (std::size_t block = 0; block < blocks_per_client; ++block) {
                const std::uint64_t id = client_base + static_cast<std::uint64_t>(block) * 4U;
                const std::uint64_t request = static_cast<std::uint64_t>(block) * 4U;
                measurements[client_index].push_back(client.round_trip(
                    submit_frame(request, id, Side::Sell, 100, 2), RequestId{request}));
                measurements[client_index].push_back(client.round_trip(
                    submit_frame(request + 1U, id + 1U, Side::Buy, 100, 1), RequestId{request + 1U}));
                measurements[client_index].push_back(client.round_trip(
                    submit_frame(request + 2U, id + 2U, Side::Buy, 90, 1), RequestId{request + 2U}));
                measurements[client_index].push_back(client.round_trip(
                    cancel_frame(request + 3U, id + 2U), RequestId{request + 3U}));
            }
        });
    }
    ready.arrive_and_wait();
    const auto started = Clock::now();
    for (auto& client : clients) client.join();
    const auto elapsed = std::chrono::duration_cast<Nanoseconds>(Clock::now() - started);
    std::vector<Nanoseconds> flattened;
    for (auto& client_measurements : measurements) {
        flattened.insert(flattened.end(), client_measurements.begin(), client_measurements.end());
    }
    const auto batch_stats = exchange.stop_and_verify(client_count * operations_per_client);
    return ThroughputSummary{.summary = summarize(std::move(flattened), elapsed), .batch_stats = batch_stats};
}

void print_batch_stats(std::size_t max_durable_batch_size, const testing::ExchangeBatchStats& stats) {
    const double mean = stats.batches_processed == 0U ? 0.0
        : static_cast<double>(stats.commands_processed) / static_cast<double>(stats.batches_processed);
    std::cout << "batch_stats," << max_durable_batch_size << ',' << stats.commands_processed << ','
              << stats.batches_processed << ',' << mean << ',';
    bool first = true;
    for (std::size_t size = 1; size < stats.batch_size_histogram.size(); ++size) {
        if (stats.batch_size_histogram[size] == 0U) continue;
        if (!first) std::cout << ';';
        std::cout << size << ':' << stats.batch_size_histogram[size];
        first = false;
    }
    std::cout << '\n';
}

[[nodiscard]] std::size_t parse_size_argument(int argc, char** argv, std::string_view name, std::size_t fallback) {
    const std::string prefix = "--" + std::string{name} + "=";
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        if (!argument.starts_with(prefix)) continue;
        std::size_t value{};
        const auto [pointer, error] = std::from_chars(
            argument.data() + static_cast<std::ptrdiff_t>(prefix.size()), argument.data() + argument.size(), value);
        if (error != std::errc{} || pointer != argument.data() + argument.size() || value == 0) std::abort();
        return value;
    }
    return fallback;
}

[[nodiscard]] std::string_view parse_string_argument(
    int argc, char** argv, std::string_view name, std::string_view fallback) {
    const std::string prefix = "--" + std::string{name} + "=";
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        if (argument.starts_with(prefix)) return argument.substr(prefix.size());
    }
    return fallback;
}

} // namespace
} // namespace matching

int main(int argc, char** argv) {
    const std::size_t samples = matching::parse_size_argument(argc, argv, "samples", 100);
    const std::size_t operations_per_client = matching::parse_size_argument(argc, argv, "operations-per-client", 32);
    const std::size_t max_durable_batch_size = matching::parse_size_argument(argc, argv, "max-durable-batch-size", 1);
    const std::string_view throughput_workload = matching::parse_string_argument(
        argc, argv, "throughput-workload", "passive");
    if (throughput_workload != "passive" && throughput_workload != "mixed") std::abort();
    std::cout << "workload,samples,p50_us,p95_us,p99_us,operations_per_second\n";
    const auto direct_passive = matching::direct_passive(samples);
    const auto direct_aggressive = matching::direct_aggressive(samples);
    const auto direct_cancel = matching::direct_cancel(samples);
    const auto direct_rejected = matching::direct_rejected(samples);
    const auto direct_mixed = matching::direct_mixed(samples);
    const auto tcp_passive = matching::tcp_passive(samples);
    const auto tcp_aggressive = matching::tcp_aggressive(samples);
    const auto tcp_cancel = matching::tcp_cancel(samples);
    const auto tcp_rejected = matching::tcp_rejected(samples);
    const auto tcp_mixed = matching::tcp_mixed(samples);
    matching::print_summary("durable_direct_passive", direct_passive);
    matching::print_summary("durable_direct_aggressive", direct_aggressive);
    matching::print_summary("durable_direct_cancel", direct_cancel);
    matching::print_summary("durable_direct_rejected", direct_rejected);
    matching::print_summary("durable_direct_mixed", direct_mixed);
    matching::print_summary("tcp_passive", tcp_passive);
    matching::print_summary("tcp_aggressive", tcp_aggressive);
    matching::print_summary("tcp_cancel", tcp_cancel);
    matching::print_summary("tcp_rejected", tcp_rejected);
    matching::print_summary("tcp_mixed", tcp_mixed);
    const std::vector<std::size_t> client_counts = throughput_workload == "passive"
        ? std::vector<std::size_t>{1U, 2U, 4U, 8U, 16U}
        : std::vector<std::size_t>{4U, 16U};
    for (const std::size_t clients : client_counts) {
        const auto throughput = throughput_workload == "passive"
            ? matching::tcp_passive_throughput(clients, operations_per_client, max_durable_batch_size)
            : matching::tcp_mixed_throughput(clients, operations_per_client, max_durable_batch_size);
        matching::print_summary(
            "tcp_" + std::string(throughput_workload) + "_batch_" + std::to_string(max_durable_batch_size) + "_" +
                std::to_string(clients) + "_clients",
            throughput.summary);
        matching::print_batch_stats(max_durable_batch_size, throughput.batch_stats);
    }
    return 0;
}
