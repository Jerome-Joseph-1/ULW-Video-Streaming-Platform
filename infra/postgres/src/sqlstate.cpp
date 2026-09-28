#include "sqlstate.hpp"

namespace infra::postgres {

DbError classify(std::string_view sqlstate) noexcept {
    if (sqlstate.size() != 5) {
        return DbError::Rejected;
    }
    if (sqlstate == "23505") {
        return DbError::Duplicate;
    }
    if (sqlstate == "40001" || sqlstate == "40P01") {
        return DbError::Retry;
    }
    if (sqlstate == "57014") {
        return DbError::Timeout;
    }
    if (sqlstate == "55P03") {
        return DbError::LockTimeout;
    }
    const std::string_view cls = sqlstate.substr(0, 2);
    // The rest of class 57 is the server ending the session: admin or crash shutdown, a dropped
    // database, idle_session_timeout. Each arrives as the last message before the socket closes.
    if (cls == "08" || cls == "57") {
        return DbError::ConnectionLost;
    }
    if (cls == "23") {
        return DbError::Constraint;
    }
    return DbError::Rejected;
}

DbError error_of(const PGresult* result, const PGconn* conn) noexcept {
    const char* sqlstate =
        result != nullptr ? PQresultErrorField(result, PG_DIAG_SQLSTATE) : nullptr;
    if (sqlstate != nullptr) {
        return classify(sqlstate);
    }
    // Errors libpq raises itself (a reset socket, a protocol violation) carry no SQLSTATE.
    return PQstatus(conn) == CONNECTION_BAD ? DbError::ConnectionLost : DbError::Rejected;
}

} // namespace infra::postgres
