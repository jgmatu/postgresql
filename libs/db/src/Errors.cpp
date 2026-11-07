#include <db/Errors.hpp>

#include <pqxx/except>

namespace pgdemo::db {

std::string_view sqlstate_of(const std::exception& e) noexcept
{
    if (const auto* f = dynamic_cast<const pqxx::failure*>(&e)) return f->sqlstate();
    return {};
}

DbErrorKind classify(const std::exception& e) noexcept
{
    if (dynamic_cast<const pqxx::broken_connection*>(&e) != nullptr) return DbErrorKind::Unavailable;

    const std::string_view state = sqlstate_of(e);
    if (state == "23505") return DbErrorKind::Conflict;
    if (state == "42501") return DbErrorKind::Forbidden;
    if (state.starts_with("23") || state.starts_with("22")) return DbErrorKind::Constraint;
    if (state.starts_with("08")) return DbErrorKind::Unavailable;
    return DbErrorKind::Other;
}

} // namespace pgdemo::db
