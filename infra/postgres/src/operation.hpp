#pragma once

#include "params.hpp"
#include "result.hpp"
#include "sql.hpp"
#include "sqlstate.hpp"

#include <expected>
#include <functional>
#include <optional>
#include <utility>

namespace infra::postgres {

using Outcome = std::expected<Result, DbError>;

struct Statement {
    Sql sql;
    Params params;
};

// Work for one pooled session: statements sent one at a time, each chosen after the outcome of
// the one before, all on the same session so that BEGIN ... COMMIT spans them. If the operation
// finishes with a transaction still open (it bailed out halfway), the pool rolls it back.
// Every member runs on the reactor thread. The result is reported exactly once, either from
// next() returning nullopt or from abandon().
class Operation {
public:
    virtual ~Operation() = default;

    // Also called to run the operation again from the top after a serialization failure or a
    // deadlock, so it must reset whatever next() has been tracking.
    [[nodiscard]] virtual Statement start() noexcept = 0;
    [[nodiscard]] virtual std::optional<Statement> next(Outcome outcome) noexcept = 0;
    // The session was lost or the deadline passed. Nothing more will be asked.
    virtual void abandon(DbError error) noexcept = 0;
};

// A single statement whose outcome goes to `done`. Its parameters must not borrow: the query
// keeps the statement for reruns, and moves on its way into the pool.
class Query final : public Operation {
public:
    using Done = std::move_only_function<void(Outcome) noexcept>;

    Query(Statement statement, Done done) noexcept
        : statement_(statement), done_(std::move(done)) {}

    [[nodiscard]] Statement start() noexcept override { return statement_; }
    [[nodiscard]] std::optional<Statement> next(Outcome outcome) noexcept override {
        done_(std::move(outcome));
        return std::nullopt;
    }
    void abandon(DbError error) noexcept override { done_(std::unexpected(error)); }

private:
    Statement statement_;
    Done done_;
};

} // namespace infra::postgres
