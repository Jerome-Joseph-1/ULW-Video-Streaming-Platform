#pragma once

#include <libpq-fe.h>
#include <memory>

namespace infra::postgres {

struct ConnCloser {
    void operator()(PGconn* conn) const noexcept { PQfinish(conn); }
};

struct ResultClearer {
    void operator()(PGresult* result) const noexcept { PQclear(result); }
};

struct NotifyFreer {
    void operator()(PGnotify* notify) const noexcept { PQfreemem(notify); }
};

struct ConninfoFreer {
    void operator()(PQconninfoOption* options) const noexcept { PQconninfoFree(options); }
};

using ConnHandle = std::unique_ptr<PGconn, ConnCloser>;
using ResultHandle = std::unique_ptr<PGresult, ResultClearer>;
using NotifyHandle = std::unique_ptr<PGnotify, NotifyFreer>;
using ConninfoHandle = std::unique_ptr<PQconninfoOption, ConninfoFreer>;

// libpq hands notices (warnings, "skipping" chatter from IF NOT EXISTS) to a processor that
// prints them to stderr unless replaced. None of them changes what a statement returned.
inline void ignore_notices(PGconn* conn) noexcept {
    PQsetNoticeProcessor(conn, [](void* /*arg*/, const char* /*message*/) {}, nullptr);
}

} // namespace infra::postgres
