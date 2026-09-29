#include "infra/postgres/health_check.hpp"

#include "result.hpp"
#include "sql.hpp"
#include "sync_connection.hpp"

#include <utility>

namespace infra::postgres {

namespace {

// Due jobs only: one waiting out a retry backoff is not late. jobs_claimable makes this an
// index scan over the queued rows.
constexpr Sql kOldestQueued = R"sql(
SELECT floor(extract(epoch FROM now() - min(run_after)))::bigint
  FROM jobs WHERE state = 'queued' AND run_after <= now())sql";

// 2 s: the database answering slower than that is not ready to serve requests either, and the
// probe runs every 5 s, so a stuck statement gives up before the next one starts.
constexpr SessionSettings kProbeSession{.application_name = "ulw-health",
                                        .statement_timeout = core::Millis{2'000}};

} // namespace

class PgHealthCheck::Impl {
public:
    explicit Impl(std::string conninfo) : conninfo_(std::move(conninfo)) {}

    std::expected<std::optional<core::Seconds>, std::string> oldest_queued_job() {
        if (!conn_ || conn_->broken()) {
            conn_.reset();
            auto opened = SyncConnection::open(conninfo_, kProbeSession);
            if (!opened) {
                return std::unexpected(std::move(opened.error().message));
            }
            conn_.emplace(std::move(*opened));
        }
        auto result = conn_->exec(kOldestQueued);
        if (!result) {
            return std::unexpected(std::move(result.error().message));
        }
        const auto cell = result->get(0, 0);
        if (!cell) {
            return std::nullopt;
        }
        const auto seconds = parse_int64(*cell);
        if (!seconds) {
            return std::unexpected("unreadable job age");
        }
        return core::Seconds{*seconds};
    }

private:
    std::string conninfo_;
    std::optional<SyncConnection> conn_;
};

PgHealthCheck::PgHealthCheck(std::string conninfo)
    : impl_(std::make_unique<Impl>(std::move(conninfo))) {}

PgHealthCheck::~PgHealthCheck() = default;

std::expected<std::optional<core::Seconds>, std::string> PgHealthCheck::oldest_queued_job() {
    return impl_->oldest_queued_job();
}

} // namespace infra::postgres
