#pragma once

#include <exception>
#include <optional>
#include <utility>
#include <variant>

#include <boost/asio/awaitable.hpp>

namespace pgdemo::wire {

/**
 * @brief Run `op` and return instead of throwing: its value, or the exception it ended with.
 *
 * Boost.Asio's `a || b` stops at the first branch that *succeeds*: a branch
 * that throws lets the others run on. Wrapped in `settle()`, every branch
 * succeeds, so `settle(a) || settle(b)` stops at the first one to end, for
 * whatever reason, and cancels the rest.
 */
template <typename T>
boost::asio::awaitable<std::pair<std::optional<T>, std::exception_ptr>> settle(boost::asio::awaitable<T> op)
{
    try
    {
        co_return std::pair<std::optional<T>, std::exception_ptr>{co_await std::move(op), nullptr};
    }
    catch (...)
    {
        co_return std::pair<std::optional<T>, std::exception_ptr>{std::nullopt, std::current_exception()};
    }
}

/// `settle()` for `void`: the exception `op` ended with, or null.
inline boost::asio::awaitable<std::exception_ptr> settle(boost::asio::awaitable<void> op)
{
    try
    {
        co_await std::move(op);
        co_return nullptr;
    }
    catch (...)
    {
        co_return std::current_exception();
    }
}

/// Rethrow the exception of whichever `settle()`d `void` branch won `||`.
template <typename... E>
void rethrow_winner(const std::variant<E...>& winner)
{
    const std::exception_ptr e = std::visit([](const std::exception_ptr& p) { return p; }, winner);
    if (e) std::rethrow_exception(e);
}

} // namespace pgdemo::wire
