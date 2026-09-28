#include "infra/postgres/migrator.hpp"

#include "params.hpp"
#include "result.hpp"
#include "sync_connection.hpp"

#include <algorithm>
#include <format>
#include <map>
#include <utility>

namespace infra::postgres {

namespace {

constexpr Sql kCreateLedger = R"sql(
CREATE TABLE IF NOT EXISTS schema_migrations (
    version    integer PRIMARY KEY,
    name       text NOT NULL,
    applied_at timestamptz NOT NULL DEFAULT now()))sql";

constexpr Sql kLedgerExists = "SELECT to_regclass('schema_migrations') IS NOT NULL";

constexpr Sql kApplied = "SELECT version, name, applied_at::text FROM schema_migrations";

constexpr Sql kRecord = "INSERT INTO schema_migrations (version, name) VALUES ($1, $2)";

// The two-int form of the advisory lock keys a space of its own, apart from the bigint keys
// that upload claims take. 0x756c77 spells "ulw".
constexpr Sql kTryLock = "SELECT pg_try_advisory_lock(7695479, 1)";
constexpr Sql kUnlock = "SELECT pg_advisory_unlock(7695479, 1)";

// A DDL statement waiting for a lock queues every later query on that table behind it. 5 s
// caps that stall at what a catalog call tolerates (its request timeout) before failing the
// migration, which is then rerun when the blocking transaction is gone.
constexpr Sql kLockTimeout = "SET LOCAL lock_timeout = '5s'";

struct Applied {
    std::string name;
    std::string applied_at;
};

MigrationError failed(std::string_view what, const DbFailure& failure) {
    return MigrationError{std::format("{}: {}", what, failure.message)};
}

} // namespace

class Migrator::Impl {
public:
    explicit Impl(SyncConnection conn) noexcept : conn_(std::move(conn)) {}

    std::expected<std::map<int, Applied>, MigrationError> applied() {
        auto exists = conn_.exec(kLedgerExists);
        if (!exists) {
            return std::unexpected(failed("reading schema_migrations", exists.error()));
        }
        std::map<int, Applied> out;
        if (exists->get(0, 0) != "t") {
            return out;
        }
        auto rows = conn_.exec(kApplied);
        if (!rows) {
            return std::unexpected(failed("reading schema_migrations", rows.error()));
        }
        for (int i = 0; i < rows->rows(); ++i) {
            const auto version = rows->get(i, 0).and_then(parse_int64);
            if (!version) {
                return std::unexpected(MigrationError{"schema_migrations holds a bad version"});
            }
            out.emplace(static_cast<int>(*version),
                        Applied{.name = std::string{rows->get(i, 1).value_or("")},
                                .applied_at = std::string{rows->get(i, 2).value_or("")}});
        }
        return out;
    }

    std::expected<void, MigrationError> run(const Migration& m) {
        const std::string label = std::format("{:04}_{}", m.version, m.name);
        auto tx = Transaction::begin(conn_);
        if (!tx) {
            return std::unexpected(failed(label, tx.error()));
        }
        if (auto r = conn_.exec(kLockTimeout); !r) {
            return std::unexpected(failed(label, r.error()));
        }
        if (auto r = conn_.run_script(m.sql); !r) {
            return std::unexpected(failed(label, r.error()));
        }
        if (auto r = conn_.exec(kRecord, Params{}.add_int(m.version).add_text(m.name)); !r) {
            return std::unexpected(failed(label, r.error()));
        }
        if (auto r = tx->commit(); !r) {
            return std::unexpected(failed(label, r.error()));
        }
        return {};
    }

    std::expected<std::vector<int>, MigrationError> apply(std::span<const Migration> known) {
        auto locked = conn_.exec(kTryLock);
        if (!locked) {
            return std::unexpected(failed("taking the migration lock", locked.error()));
        }
        // Waiting would only find the work done, or stack this deploy behind a stuck one.
        if (locked->get(0, 0) != "t") {
            return std::unexpected(MigrationError{"another migrator is running"});
        }
        auto result = apply_locked(known);
        // Ending the session would release it too; this keeps a reused session clean.
        [[maybe_unused]] const auto unlocked = conn_.exec(kUnlock);
        return result;
    }

private:
    std::expected<std::vector<int>, MigrationError> apply_locked(std::span<const Migration> known) {
        if (auto r = conn_.exec(kCreateLedger); !r) {
            return std::unexpected(failed("creating schema_migrations", r.error()));
        }
        auto done = applied();
        if (!done) {
            return std::unexpected(std::move(done.error()));
        }
        const int newest = done->empty() ? 0 : done->rbegin()->first;
        std::vector<const Migration*> pending;
        for (const Migration& m : known) {
            if (done->contains(m.version)) {
                continue;
            }
            if (m.version < newest) {
                return std::unexpected(MigrationError{std::format(
                    "{:04}_{} is older than applied version {:04}; migrations only move forward",
                    m.version, m.name, newest)});
            }
            pending.push_back(&m);
        }
        std::ranges::sort(pending, {}, &Migration::version);
        std::vector<int> ran;
        for (const Migration* m : pending) {
            if (auto r = run(*m); !r) {
                return std::unexpected(std::move(r.error()));
            }
            ran.push_back(m->version);
        }
        return ran;
    }

    SyncConnection conn_;
};

Migrator::Migrator(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
Migrator::~Migrator() = default;
Migrator::Migrator(Migrator&&) noexcept = default;
Migrator& Migrator::operator=(Migrator&&) noexcept = default;

std::expected<Migrator, MigrationError> Migrator::connect(const std::string& conninfo) {
    auto conn = SyncConnection::open(conninfo);
    if (!conn) {
        return std::unexpected(failed("connecting", conn.error()));
    }
    return Migrator{std::make_unique<Impl>(std::move(*conn))};
}

std::expected<std::vector<MigrationStatus>, MigrationError>
Migrator::status(std::span<const Migration> known) {
    auto done = impl_->applied();
    if (!done) {
        return std::unexpected(std::move(done.error()));
    }
    std::map<int, MigrationStatus> merged;
    for (const Migration& m : known) {
        merged.emplace(m.version, MigrationStatus{.version = m.version,
                                                  .name = std::string{m.name},
                                                  .applied_at = std::nullopt,
                                                  .unknown = false});
    }
    for (auto& [version, applied] : *done) {
        MigrationStatus& entry = merged
                                     .try_emplace(version, MigrationStatus{.version = version,
                                                                           .name = applied.name,
                                                                           .applied_at = {},
                                                                           .unknown = true})
                                     .first->second;
        entry.applied_at = std::move(applied.applied_at);
    }
    std::vector<MigrationStatus> out;
    out.reserve(merged.size());
    for (auto& [version, entry] : merged) {
        out.push_back(std::move(entry));
    }
    return out;
}

std::expected<std::vector<int>, MigrationError> Migrator::apply(std::span<const Migration> known) {
    return impl_->apply(known);
}

} // namespace infra::postgres
