#include "sync_connection.hpp"

#include "conninfo.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <climits>
#include <poll.h>

namespace infra::postgres {

namespace {

std::string trimmed(const char* message) {
    std::string out = message != nullptr ? message : "";
    while (!out.empty() && (out.back() == '\n' || out.back() == ' ')) {
        out.pop_back();
    }
    return out;
}

// libpq's message for a connection that failed names the host, the user and the database from
// the connection string, and is only ever one bug away from more of it. What it did is said
// in words of our own instead.
std::string connect_failure(const PGconn* conn) {
    if (PQconnectionNeedsPassword(conn) != 0) {
        return "the server asked for a password the connection string does not give";
    }
    if (PQconnectionUsedPassword(conn) != 0) {
        return "the server refused the login";
    }
    return "the server did not accept a connection";
}

} // namespace

std::expected<SyncConnection, DbFailure> SyncConnection::open(const std::string& conninfo,
                                                              const SessionSettings& settings) {
    const auto target = parse_conninfo(conninfo);
    if (!target) {
        return std::unexpected(DbFailure{.error = DbError::Rejected, .message = target.error()});
    }
    const std::string options = session_options(settings.statement_timeout, target->options);
    // Entries before dbname are defaults the connection string may override; options after it
    // already carries the string's own. Without connect_timeout a dead host holds
    // PQconnectdbParams for the kernel's TCP timeout, minutes; 5 s is dozens of LAN round trips.
    // Four settings besides the keepalives, and the null pair that ends the list.
    constexpr std::size_t kEntries = 4 + kKeepalives.size() + 1;
    std::array<const char*, kEntries> keywords{};
    std::array<const char*, kEntries> values{};
    std::size_t n = 0;
    const auto add = [&](const char* keyword, const char* value) {
        keywords.at(n) = keyword;
        values.at(n) = value;
        ++n;
    };
    add("fallback_application_name", settings.application_name);
    add("connect_timeout", "5");
    for (const auto& [keyword, value] : kKeepalives) {
        add(keyword, value);
    }
    add("dbname", conninfo.c_str());
    add("options", options.c_str());
    ConnHandle conn{PQconnectdbParams(keywords.data(), values.data(), 1)};
    if (!conn) {
        return std::unexpected(
            DbFailure{.error = DbError::ConnectionLost, .message = "out of memory"});
    }
    if (PQstatus(conn.get()) != CONNECTION_OK) {
        return std::unexpected(
            DbFailure{.error = DbError::ConnectionLost, .message = connect_failure(conn.get())});
    }
    ignore_notices(conn.get());
    return SyncConnection{std::move(conn)};
}

DbFailure SyncConnection::failure(const PGresult* result) const {
    const char* message = result != nullptr ? PQresultErrorMessage(result) : nullptr;
    if (message == nullptr || *message == '\0') {
        message = PQerrorMessage(conn_.get());
    }
    return DbFailure{.error = error_of(result, conn_.get()), .message = trimmed(message)};
}

std::expected<Result, DbFailure> SyncConnection::exec(Sql sql, const Params& params) {
    const Params::Wire wire = params.wire();
    ResultHandle result{PQexecParams(conn_.get(), sql.c_str(), wire.count, wire.types.data(),
                                     wire.values.data(), wire.lengths.data(), wire.formats.data(),
                                     0)};
    const ExecStatusType status = result ? PQresultStatus(result.get()) : PGRES_FATAL_ERROR;
    if (status != PGRES_COMMAND_OK && status != PGRES_TUPLES_OK) {
        return std::unexpected(failure(result.get()));
    }
    return Result{std::move(result)};
}

std::expected<void, DbFailure> SyncConnection::run_script(const char* script) {
    const ResultHandle result{PQexec(conn_.get(), script)};
    const ExecStatusType status = result ? PQresultStatus(result.get()) : PGRES_FATAL_ERROR;
    if (status != PGRES_COMMAND_OK && status != PGRES_TUPLES_OK) {
        return std::unexpected(failure(result.get()));
    }
    return {};
}

bool SyncConnection::take_notifications() noexcept {
    bool any = false;
    for (NotifyHandle n{PQnotifies(conn_.get())}; n; n.reset(PQnotifies(conn_.get()))) {
        any = true;
    }
    return any;
}

std::expected<bool, DbFailure> SyncConnection::wait_for_notification(core::Millis timeout) {
    // Notifications that arrived during earlier statements are already queued inside libpq.
    if (take_notifications()) {
        return true;
    }
    pollfd pfd{.fd = PQsocket(conn_.get()), .events = POLLIN, .revents = 0};
    const auto wait_ms =
        static_cast<int>(std::clamp<core::Millis::rep>(timeout.count(), 0, INT_MAX));
    const int ready = ::poll(&pfd, 1, wait_ms);
    if (ready < 0) {
        // An interrupted wait is a spurious wakeup; the caller loops anyway.
        return errno == EINTR ? std::expected<bool, DbFailure>{false}
                              : std::unexpected(DbFailure{.error = DbError::ConnectionLost,
                                                          .message = "poll failed"});
    }
    if (ready == 0) {
        return false;
    }
    if (PQconsumeInput(conn_.get()) == 0) {
        return std::unexpected(failure(nullptr));
    }
    return take_notifications();
}

std::expected<Transaction, DbFailure> Transaction::begin(SyncConnection& conn) {
    if (auto r = conn.exec("BEGIN"); !r) {
        return std::unexpected(std::move(r.error()));
    }
    return Transaction{conn};
}

Transaction::~Transaction() {
    if (conn_ != nullptr) {
        // A failed rollback leaves the session broken, and broken() says so to the owner.
        [[maybe_unused]] const auto rolled_back = conn_->exec("ROLLBACK");
    }
}

std::expected<void, DbFailure> Transaction::commit() {
    SyncConnection* conn = std::exchange(conn_, nullptr);
    if (auto r = conn->exec("COMMIT"); !r) {
        return std::unexpected(std::move(r.error()));
    }
    return {};
}

} // namespace infra::postgres
