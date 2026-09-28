#pragma once

#include <cstdint>
#include <libpq-fe.h>
#include <string_view>

namespace infra::postgres {

enum class DbError : std::uint8_t {
    // 23505: the row exists. Expected wherever an insert doubles as an idempotency check.
    Duplicate,
    // 40001, 40P01: a serialization failure or deadlock rolled the transaction back; running it
    // again is safe.
    Retry,
    // Class 08, the shutdown family of class 57, or a socket failure: the session is gone and
    // the connection must be replaced.
    ConnectionLost,
    // 57014: statement_timeout (or a cancel) stopped the statement. The session survives.
    Timeout,
    // 55P03: lock_timeout expired, or a NOWAIT lock was taken.
    LockTimeout,
    // Class 23 other than 23505: a constraint refused a write the code should never issue.
    Constraint,
    // Anything else: the statement is wrong (syntax, types, privileges) or the server is out of
    // a resource. Sending it again unchanged will not help.
    Rejected,
};

[[nodiscard]] DbError classify(std::string_view sqlstate) noexcept;

// The error an unsuccessful result stands for. `result` may be null: libpq returns no result
// at all when the connection broke before one could be built.
[[nodiscard]] DbError error_of(const PGresult* result, const PGconn* conn) noexcept;

} // namespace infra::postgres
