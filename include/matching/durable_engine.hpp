#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <variant>
#include <vector>

#include "matching/journal_storage.hpp"
#include "matching/order_book.hpp"

namespace matching {

enum class DurableEngineErrorCode : std::uint8_t {
    JournalFailure,
    ApplicationFailure,
    RecoveryReplayFailure,
    EngineFailed,
};

struct DurableEngineError {
    DurableEngineErrorCode code;
    std::optional<JournalStorageError> journal_error{};
};

enum class DurableEngineState : std::uint8_t {
    Healthy,
    Failed,
};

class DurableEngine {
public:
    DurableEngine(const DurableEngine&) = delete;
    DurableEngine& operator=(const DurableEngine&) = delete;
    DurableEngine(DurableEngine&&) noexcept = default;
    DurableEngine& operator=(DurableEngine&&) noexcept = default;
    ~DurableEngine() = default;

    using CreateResult = std::variant<DurableEngine, DurableEngineError>;
    using SubmitCommandResult = std::variant<SubmitResult, DurableEngineError>;
    using CancelCommandResult = std::variant<CancelResult, DurableEngineError>;

    // Creates a new journal exclusively and pairs it with an empty OrderBook.
    [[nodiscard]] static CreateResult create(const std::filesystem::path& path);

    // Rebuilds a fresh OrderBook from the journal without appending replayed commands.
    [[nodiscard]] static CreateResult recover(const std::filesystem::path& path);

    // Persists and fsyncs the command before applying it to the OrderBook.
    [[nodiscard]] SubmitCommandResult submit(NewLimitOrder order);
    [[nodiscard]] CancelCommandResult cancel(OrderId id);

    [[nodiscard]] const OrderBook& order_book() const noexcept;
    [[nodiscard]] DurableEngineState state() const noexcept;

private:
    DurableEngine(std::unique_ptr<OrderBook> order_book, JournalWriter writer) noexcept;

    [[nodiscard]] static std::optional<DurableEngineError> replay(
        OrderBook& order_book,
        const std::vector<JournalRecord>& records) noexcept;

    std::unique_ptr<OrderBook> order_book_;
    JournalWriter writer_;
    DurableEngineState state_{DurableEngineState::Healthy};
};

} // namespace matching
