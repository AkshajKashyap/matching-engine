#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>

namespace matching {

enum class Side : std::uint8_t {
    Buy,
    Sell,
};

struct OrderId {
    std::uint64_t value{};

    constexpr explicit OrderId(std::uint64_t value_in = 0) noexcept : value(value_in) {}

    constexpr auto operator<=>(const OrderId&) const = default;
};

struct TradeId {
    std::uint64_t value{};

    constexpr explicit TradeId(std::uint64_t value_in = 0) noexcept : value(value_in) {}

    constexpr auto operator<=>(const TradeId&) const = default;
};

// Price is expressed in an instrument's integer tick units.
struct Price {
    std::uint64_t ticks{};

    constexpr explicit Price(std::uint64_t ticks_in = 0) noexcept : ticks(ticks_in) {}

    constexpr auto operator<=>(const Price&) const = default;
};

// Quantity is expressed in integer units. Lot-size validation is deferred.
struct Quantity {
    std::uint64_t units{};

    constexpr explicit Quantity(std::uint64_t units_in = 0) noexcept : units(units_in) {}

    constexpr auto operator<=>(const Quantity&) const = default;
};

template <typename StrongUnsigned>
struct StrongUnsignedHash {
    [[nodiscard]] std::size_t operator()(const StrongUnsigned& value) const noexcept {
        return std::hash<std::uint64_t>{}(value.value);
    }
};

using OrderIdHash = StrongUnsignedHash<OrderId>;
using TradeIdHash = StrongUnsignedHash<TradeId>;

} // namespace matching
