#include "matching/durable_engine.hpp"

#include <exception>
#include <type_traits>
#include <utility>

namespace matching {
namespace {

[[nodiscard]] DurableEngineError journal_failure(JournalStorageError error) {
    return DurableEngineError{
        .code = DurableEngineErrorCode::JournalFailure,
        .journal_error = std::move(error),
    };
}

[[nodiscard]] DurableEngineError application_failure(DurableEngineErrorCode code) {
    return DurableEngineError{
        .code = code,
        .journal_error = std::nullopt,
    };
}

void apply_record(OrderBook& order_book, const JournalRecord& record) {
    std::visit(
        [&order_book](const auto& command) {
            using Command = std::decay_t<decltype(command)>;
            if constexpr (std::is_same_v<Command, SubmitLimitOrderCommand>) {
                static_cast<void>(order_book.submit(command.order));
            } else {
                static_cast<void>(order_book.cancel(command.order_id));
            }
        },
        record.command);
}

} // namespace

DurableEngine::DurableEngine(std::unique_ptr<OrderBook> order_book, JournalWriter writer) noexcept
    : order_book_(std::move(order_book)), writer_(std::move(writer)) {}

DurableEngine::CreateResult DurableEngine::create(const std::filesystem::path& path) {
    auto writer_result = JournalWriter::create(path);
    if (const auto* error = std::get_if<JournalStorageError>(&writer_result)) {
        return journal_failure(*error);
    }
    try {
        return DurableEngine{
            std::make_unique<OrderBook>(),
            std::move(std::get<JournalWriter>(writer_result)),
        };
    } catch (...) {
        return application_failure(DurableEngineErrorCode::ApplicationFailure);
    }
}

DurableEngine::CreateResult DurableEngine::recover(const std::filesystem::path& path) {
    try {
        JournalScanResult scan_result = JournalReader::scan(path);
        if (const auto* error = std::get_if<JournalStorageError>(&scan_result)) {
            return journal_failure(*error);
        }

        std::optional<JournalWriter> repaired_writer;
        if (std::get<JournalScan>(scan_result).has_truncated_tail) {
            auto repair_result = JournalWriter::repair_truncated_tail_and_open(path);
            if (const auto* error = std::get_if<JournalStorageError>(&repair_result)) {
                return journal_failure(*error);
            }
            repaired_writer.emplace(std::move(std::get<JournalWriter>(repair_result)));

            scan_result = JournalReader::scan(path);
            if (const auto* error = std::get_if<JournalStorageError>(&scan_result)) {
                return journal_failure(*error);
            }
            if (std::get<JournalScan>(scan_result).has_truncated_tail) {
                return application_failure(DurableEngineErrorCode::RecoveryReplayFailure);
            }
        }
        auto order_book = std::make_unique<OrderBook>();
        const JournalScan& scan = std::get<JournalScan>(scan_result);
        if (const auto replay_error = replay(*order_book, scan.records)) {
            return *replay_error;
        }

        if (repaired_writer.has_value()) {
            return DurableEngine{std::move(order_book), std::move(*repaired_writer)};
        }

        auto writer_result = JournalWriter::open_recovered(path);
        if (const auto* error = std::get_if<JournalStorageError>(&writer_result)) {
            return journal_failure(*error);
        }
        return DurableEngine{std::move(order_book), std::move(std::get<JournalWriter>(writer_result))};
    } catch (...) {
        return application_failure(DurableEngineErrorCode::RecoveryReplayFailure);
    }
}

DurableEngine::SubmitCommandResult DurableEngine::submit(NewLimitOrder order) {
    if (state_ == DurableEngineState::Failed) {
        return application_failure(DurableEngineErrorCode::EngineFailed);
    }

    const JournalAppendResult append_result = writer_.append_and_sync(
        SubmitLimitOrderCommand{.order = order});
    if (const auto* error = std::get_if<JournalStorageError>(&append_result)) {
        state_ = DurableEngineState::Failed;
        return journal_failure(*error);
    }

    try {
        return order_book_->submit(order);
    } catch (...) {
        state_ = DurableEngineState::Failed;
        return application_failure(DurableEngineErrorCode::ApplicationFailure);
    }
}

DurableEngine::CancelCommandResult DurableEngine::cancel(OrderId id) {
    if (state_ == DurableEngineState::Failed) {
        return application_failure(DurableEngineErrorCode::EngineFailed);
    }

    const JournalAppendResult append_result = writer_.append_and_sync(
        CancelOrderCommand{.order_id = id});
    if (const auto* error = std::get_if<JournalStorageError>(&append_result)) {
        state_ = DurableEngineState::Failed;
        return journal_failure(*error);
    }

    try {
        return order_book_->cancel(id);
    } catch (...) {
        state_ = DurableEngineState::Failed;
        return application_failure(DurableEngineErrorCode::ApplicationFailure);
    }
}

const OrderBook& DurableEngine::order_book() const noexcept {
    return *order_book_;
}

DurableEngineState DurableEngine::state() const noexcept {
    return state_;
}

std::optional<DurableEngineError> DurableEngine::replay(
    OrderBook& order_book,
    const std::vector<JournalRecord>& records) noexcept {
    try {
        for (const JournalRecord& record : records) {
            apply_record(order_book, record);
        }
    } catch (...) {
        return application_failure(DurableEngineErrorCode::RecoveryReplayFailure);
    }
    return std::nullopt;
}

} // namespace matching
