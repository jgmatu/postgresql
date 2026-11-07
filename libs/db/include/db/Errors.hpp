#pragma once

#include <exception>
#include <string_view>

namespace pgdemo::db {

/// Coarse classification of a database failure, used to pick an HTTP status.
enum class DbErrorKind {
    Conflict,     ///< unique_violation (23505)
    Constraint,   ///< check / not-null / foreign-key violation, bad input (23xxx, 22xxx)
    Forbidden,    ///< insufficient_privilege (42501): the PostgreSQL role refused it
    Unavailable,  ///< connection lost or not open
    Other,
};

/// Classify the exception currently being handled.
DbErrorKind classify(const std::exception& e) noexcept;

/// SQLSTATE of a libpqxx failure, or an empty view.
std::string_view sqlstate_of(const std::exception& e) noexcept;

} // namespace pgdemo::db
