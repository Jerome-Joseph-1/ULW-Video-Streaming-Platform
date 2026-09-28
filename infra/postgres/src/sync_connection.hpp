#pragma once

#include "core/util/time.hpp"

#include "libpq_handles.hpp"
#include "params.hpp"
#include "result.hpp"
#include "sql.hpp"
#include "sqlstate.hpp"

#include <expected>
#include <string>
#include <utility>

namespace infra::postgres {

struct DbFailure {
    DbError error = DbError::Rejected;
    std::string message;
};

struct SessionSettings {
    // Names the session in pg_stat_activity unless the connection string names it.
    const char* application_name = "";
    // Also bounds how long a transaction may sit idle. Zero bounds neither, for DDL that may
    // run long.
    core::Millis statement_timeout{0};
};

// A blocking session for the worker and the migrator. Never used on a reactor thread.
class SyncConnection {
public:
    [[nodiscard]] static std::expected<SyncConnection, DbFailure>
    open(const std::string& conninfo, const SessionSettings& settings);

    [[nodiscard]] std::expected<Result, DbFailure> exec(Sql sql, const Params& params = {});
    // Simple-query protocol: `script` may hold several statements and binds nothing. For
    // migration files, which are text the build embedded rather than literals.
    [[nodiscard]] std::expected<void, DbFailure> run_script(const char* script);
    // Waits for a notification on a channel this session LISTENs to, at most `timeout`.
    // true when one arrived; several queued ones count as one.
    [[nodiscard]] std::expected<bool, DbFailure> wait_for_notification(core::Millis timeout);

    [[nodiscard]] bool broken() const noexcept { return PQstatus(conn_.get()) != CONNECTION_OK; }

private:
    explicit SyncConnection(ConnHandle conn) noexcept : conn_(std::move(conn)) {}

    [[nodiscard]] DbFailure failure(const PGresult* result) const;
    [[nodiscard]] bool take_notifications() noexcept;

    ConnHandle conn_;
};

// Rolls back on destruction unless committed, so an early return cannot leave a transaction
// open on the session.
class Transaction {
public:
    [[nodiscard]] static std::expected<Transaction, DbFailure> begin(SyncConnection& conn);
    ~Transaction();
    Transaction(Transaction&& other) noexcept : conn_(std::exchange(other.conn_, nullptr)) {}
    Transaction& operator=(Transaction&&) = delete;
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;

    [[nodiscard]] std::expected<void, DbFailure> commit();

private:
    explicit Transaction(SyncConnection& conn) noexcept : conn_(&conn) {}

    SyncConnection* conn_;
};

} // namespace infra::postgres
