#include "infra/postgres/e2ee_directory.hpp"

#include "deferred_calls.hpp"
#include "operation.hpp"
#include "pool.hpp"

#include <algorithm>
#include <limits>
#include <optional>
#include <string>
#include <utility>

namespace infra::postgres {

namespace {

using core::ports::E2eeCallback;
using core::ports::E2eeError;
using core::ports::E2eeResult;
using core::ports::FetchedKeyPackage;
using core::ports::KeyPackageBytes;
using core::ports::StoredCommit;

constexpr auto kMaxEpoch = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());

constexpr Sql kBegin = "BEGIN";
constexpr Sql kCommit = "COMMIT";

// Registrations of one user queue on this transaction-scoped lock, so the live count the insert
// reads on its fresh snapshot cannot be passed by a registration running beside it. The
// two-key form keeps these apart from upload claims, which use the single-key space; the first
// key names the purpose.
constexpr Sql kLockUser = "SELECT pg_advisory_xact_lock(3, hashtext($1))";

// A conflict, or a user at the cap, inserts nothing; the next statement, on a fresh snapshot,
// reads who holds the id. Reading it in this statement would miss a row a concurrent
// registration of the same id committed after the snapshot was taken.
constexpr Sql kInsertDevice = R"sql(
INSERT INTO devices (id, user_id)
SELECT $1, $2
 WHERE (SELECT count(*) FROM devices WHERE user_id = $2 AND revoked_at IS NULL) < $3
ON CONFLICT (id) DO NOTHING)sql";
constexpr Sql kDeviceHolder =
    "SELECT user_id = $2, revoked_at IS NOT NULL FROM devices WHERE id = $1";

// Keeps the user's newest tombstones; ids are never reused, so ties in revoked_at are harmless.
constexpr Sql kDropTombstones = R"sql(
DELETE FROM devices
 WHERE id IN (SELECT id FROM devices
               WHERE user_id = $1 AND revoked_at IS NOT NULL
               ORDER BY revoked_at DESC, id DESC
              OFFSET $2))sql";

// Retiring the device takes its row lock, which every fetch, publish and commit claim of the
// device also takes. The packages are deleted by a second statement in the same transaction,
// because only a statement started after the lock was granted sees the packages a publish that
// held the lock before us committed; under READ COMMITTED this statement's snapshot predates
// them.
constexpr Sql kRetireDevice = R"sql(
UPDATE devices SET revoked_at = coalesce(revoked_at, now())
 WHERE id = $1 AND user_id = $2)sql";
constexpr Sql kDropPackages = "DELETE FROM key_packages WHERE device_id = $1";

// Publishes of one device queue on this lock, so the count the next statement reads (on a fresh
// snapshot) cannot move under it except downwards, as fetches take packages.
constexpr Sql kLockDevice = R"sql(
SELECT revoked_at IS NOT NULL FROM devices WHERE id = $1 AND user_id = $2
   FOR NO KEY UPDATE)sql";
constexpr Sql kAddPackages = R"sql(
WITH held AS (SELECT count(*) AS n FROM key_packages WHERE device_id = $1),
added AS (
    INSERT INTO key_packages (device_id, body)
    SELECT $1, body FROM unnest($2::bytea[]) AS p(body)
     WHERE (SELECT n FROM held) + cardinality($2::bytea[]) <= $3
    RETURNING 1)
SELECT (SELECT n FROM held), (SELECT count(*) FROM added))sql";

// One statement, so a package is handed out only by the statement that deletes it.
//
// FOR SHARE on the device orders the fetch against deregistration: a retirement in progress
// makes the fetch wait, then see revoked_at set; a fetch in progress makes the retirement wait
// until the package it took is gone for good.
//
// The row lock in the subquery is what keeps concurrent fetchers from coming back empty-handed.
// Without one, every fetcher picks the same oldest row; the losers' deletes find it gone,
// READ COMMITTED rechecks only that row, and they return nothing although packages remain.
// Locked, a fetcher whose row vanished moves on to the next one. SKIP LOCKED goes further and
// has fetchers pass over rows others are taking instead of queueing on them, so a burst takes
// different packages in parallel. The price: a fetcher that skipped the last row reports
// Exhausted, and if the statement that held it then fails, the row survives unclaimed until the
// next fetch.
//
// The count reads the snapshot from before the delete, so it still includes the package taken
// here, and counts packages that concurrent fetches are taking; it is a hint for the replenish
// signal, not a balance.
constexpr Sql kFetchPackage = R"sql(
WITH device AS (
    SELECT id, revoked_at IS NOT NULL AS revoked FROM devices
     WHERE id = $1 AND user_id = $2
       FOR SHARE),
taken AS (
    DELETE FROM key_packages
     WHERE id = (SELECT id FROM key_packages
                  WHERE device_id = (SELECT id FROM device WHERE NOT revoked)
                  ORDER BY id
                    FOR UPDATE SKIP LOCKED
                  LIMIT 1)
    RETURNING body)
SELECT device.revoked, (SELECT body FROM taken),
       (SELECT count(*) FROM key_packages WHERE device_id = device.id)
  FROM device)sql";

// The primary key (room_id, epoch) settles two claims for the same epoch: the second waits for
// the first and then does nothing. The max() check refuses a claim for an epoch the room has
// not reached, so epochs are claimed in order with no gaps. The commit is stored in the row
// that claims the epoch: were the claim committed apart from the commit, a crash between the
// two would spend the epoch on a commit no member could ever receive, and wedge the room.
constexpr Sql kClaimEpoch = R"sql(
WITH device AS (
    SELECT revoked_at IS NOT NULL AS revoked FROM devices
     WHERE id = $3 AND user_id = $4
       FOR SHARE),
claimed AS (
    INSERT INTO mls_epochs (room_id, epoch, device_id, body)
    SELECT $1, $2, $3, $5
     WHERE EXISTS (SELECT 1 FROM device WHERE NOT revoked)
       AND $2 = (SELECT coalesce(max(epoch) + 1, 0) FROM mls_epochs WHERE room_id = $1)
    ON CONFLICT DO NOTHING
    RETURNING 1)
SELECT device.revoked, (SELECT count(*) FROM claimed) FROM device)sql";

constexpr Sql kFetchCommits = R"sql(
SELECT epoch, body FROM mls_epochs
 WHERE room_id = $1 AND epoch >= $2
 ORDER BY epoch
 LIMIT $3)sql";

E2eeError to_e2ee_error(DbError e) noexcept {
    switch (e) {
    case DbError::Duplicate:
    case DbError::Constraint:
        return E2eeError::Invalid;
    case DbError::Retry:
    case DbError::ConnectionLost:
    case DbError::Timeout:
    case DbError::LockTimeout:
    case DbError::Rejected:
        return E2eeError::Unavailable;
    }
    return E2eeError::Unavailable;
}

template <class T> E2eeResult<T> failure(DbError e) {
    return std::unexpected(to_e2ee_error(e));
}

// $1 the device, $2 its user: the leading parameters of every device statement.
Params device_params(const core::DeviceId& device, const core::UserId& user) noexcept {
    return Params{}.add_uuid(device.uuid()).add_text(user.view());
}

// Parameters bind the user's text by reference and a statement goes out on a later iteration,
// and again after a serialization failure, so every operation owns what it binds.
class RegisterDevice final : public Operation {
public:
    RegisterDevice(const core::UserId& user, const core::DeviceId& device,
                   E2eeCallback<void> done) noexcept
        : user_(user), device_(device), done_(std::move(done)) {}

    [[nodiscard]] Statement start() noexcept override {
        step_ = Step::Begin;
        return Statement{.sql = kBegin, .params = {}};
    }

    [[nodiscard]] std::optional<Statement> next(Outcome outcome) noexcept override {
        if (!outcome) {
            return finish(failure<void>(outcome.error()));
        }
        switch (step_) {
        case Step::Begin:
            step_ = Step::Lock;
            return Statement{.sql = kLockUser, .params = Params{}.add_text(user_.view())};
        case Step::Lock:
            step_ = Step::Insert;
            return Statement{
                .sql = kInsertDevice,
                .params = device_params(device_, user_)
                              .add_int(static_cast<std::int64_t>(core::ports::kMaxDevicesPerUser))};
        case Step::Insert:
            if (outcome->affected() == 1) {
                result_ = {};
                step_ = Step::Commit;
                return Statement{.sql = kCommit, .params = {}};
            }
            step_ = Step::Holder;
            return Statement{.sql = kDeviceHolder, .params = device_params(device_, user_)};
        case Step::Holder:
            // Nothing to commit: the transaction wrote nothing, and the pool rolls it back.
            return finish(held(*outcome));
        case Step::Commit:
            return finish(result_);
        }
        return finish(std::unexpected(E2eeError::Unavailable));
    }

    void abandon(DbError error) noexcept override { done_(failure<void>(error)); }

private:
    enum class Step : std::uint8_t { Begin, Lock, Insert, Holder, Commit };

    // Why the insert wrote nothing: the id is taken, or the user is at the cap.
    static E2eeResult<void> held(const Result& row) {
        if (row.rows() == 0) {
            return std::unexpected(E2eeError::Full);
        }
        const auto same_user = row.get(0, 0).and_then(parse_bool);
        const auto revoked = row.get(0, 1).and_then(parse_bool);
        if (!same_user || !revoked) {
            return std::unexpected(E2eeError::Corrupt);
        }
        if (!*same_user) {
            return std::unexpected(E2eeError::Conflict);
        }
        // Ours already: a replay of this registration, or of one since retired.
        if (*revoked) {
            return std::unexpected(E2eeError::Revoked);
        }
        return {};
    }

    std::optional<Statement> finish(E2eeResult<void> result) noexcept {
        done_(result);
        return std::nullopt;
    }

    core::UserId user_;
    core::DeviceId device_;
    Step step_ = Step::Begin;
    E2eeResult<void> result_;
    E2eeCallback<void> done_;
};

class DeregisterDevice final : public Operation {
public:
    DeregisterDevice(const core::UserId& user, const core::DeviceId& device,
                     E2eeCallback<void> done) noexcept
        : user_(user), device_(device), done_(std::move(done)) {}

    [[nodiscard]] Statement start() noexcept override {
        step_ = Step::Begin;
        return Statement{.sql = kBegin, .params = {}};
    }

    [[nodiscard]] std::optional<Statement> next(Outcome outcome) noexcept override {
        if (!outcome) {
            return finish(failure<void>(outcome.error()));
        }
        switch (step_) {
        case Step::Begin:
            step_ = Step::Retire;
            return Statement{.sql = kRetireDevice, .params = device_params(device_, user_)};
        case Step::Retire:
            // Retiring a retired device succeeds: it is a replay whose reply was lost.
            if (outcome->affected() != 1) {
                return finish(std::unexpected(E2eeError::NotFound));
            }
            step_ = Step::Drop;
            return Statement{.sql = kDropPackages, .params = Params{}.add_uuid(device_.uuid())};
        case Step::Drop:
            step_ = Step::Prune;
            return Statement{.sql = kDropTombstones,
                             .params = Params{}
                                           .add_text(user_.view())
                                           .add_int(static_cast<std::int64_t>(
                                               core::ports::kRetiredDevicesKept))};
        case Step::Prune:
            step_ = Step::Commit;
            return Statement{.sql = kCommit, .params = {}};
        case Step::Commit:
            return finish({});
        }
        return finish(std::unexpected(E2eeError::Unavailable));
    }

    void abandon(DbError error) noexcept override { done_(failure<void>(error)); }

private:
    enum class Step : std::uint8_t { Begin, Retire, Drop, Prune, Commit };

    std::optional<Statement> finish(E2eeResult<void> result) noexcept {
        done_(result);
        return std::nullopt;
    }

    core::UserId user_;
    core::DeviceId device_;
    Step step_ = Step::Begin;
    E2eeCallback<void> done_;
};

class PublishKeyPackages final : public Operation {
public:
    PublishKeyPackages(const core::UserId& user, const core::DeviceId& device, std::string encoded,
                       E2eeCallback<std::size_t> done) noexcept
        : user_(user), device_(device), encoded_(std::move(encoded)), done_(std::move(done)) {}

    [[nodiscard]] Statement start() noexcept override {
        step_ = Step::Begin;
        return Statement{.sql = kBegin, .params = {}};
    }

    [[nodiscard]] std::optional<Statement> next(Outcome outcome) noexcept override {
        if (!outcome) {
            return finish(failure<std::size_t>(outcome.error()));
        }
        switch (step_) {
        case Step::Begin:
            step_ = Step::Lock;
            return Statement{.sql = kLockDevice, .params = device_params(device_, user_)};
        case Step::Lock: {
            if (outcome->rows() == 0) {
                return finish(std::unexpected(E2eeError::NotFound));
            }
            const auto revoked = outcome->get(0, 0).and_then(parse_bool);
            if (!revoked) {
                return finish(std::unexpected(E2eeError::Corrupt));
            }
            if (*revoked) {
                return finish(std::unexpected(E2eeError::Revoked));
            }
            step_ = Step::Add;
            return Statement{.sql = kAddPackages,
                             .params = Params{}
                                           .add_uuid(device_.uuid())
                                           .add_bytea_array(encoded_)
                                           .add_int(static_cast<std::int64_t>(
                                               core::ports::kMaxKeyPackagesPerDevice))};
        }
        case Step::Add: {
            const auto held = outcome->get(0, 0).and_then(parse_uint64);
            const auto added = outcome->get(0, 1).and_then(parse_uint64);
            if (!held || !added) {
                return finish(std::unexpected(E2eeError::Corrupt));
            }
            if (*added == 0) {
                return finish(std::unexpected(E2eeError::Full));
            }
            total_ = *held + *added;
            step_ = Step::Commit;
            return Statement{.sql = kCommit, .params = {}};
        }
        case Step::Commit:
            return finish(total_);
        }
        return finish(std::unexpected(E2eeError::Unavailable));
    }

    void abandon(DbError error) noexcept override { done_(failure<std::size_t>(error)); }

private:
    enum class Step : std::uint8_t { Begin, Lock, Add, Commit };

    std::optional<Statement> finish(E2eeResult<std::size_t> result) noexcept {
        done_(result);
        return std::nullopt;
    }

    core::UserId user_;
    core::DeviceId device_;
    std::string encoded_;
    std::uint64_t total_ = 0;
    Step step_ = Step::Begin;
    E2eeCallback<std::size_t> done_;
};

E2eeResult<FetchedKeyPackage> decode_fetch(const Result& row) {
    if (row.rows() == 0) {
        return std::unexpected(E2eeError::NotFound);
    }
    const auto revoked = row.get(0, 0).and_then(parse_bool);
    const auto counted = row.get(0, 2).and_then(parse_uint64);
    if (!revoked || !counted) {
        return std::unexpected(E2eeError::Corrupt);
    }
    if (*revoked) {
        return std::unexpected(E2eeError::Revoked);
    }
    const auto body = row.get(0, 1);
    if (!body) {
        return std::unexpected(E2eeError::Exhausted);
    }
    auto package = parse_bytea(*body);
    if (!package || *counted == 0) {
        return std::unexpected(E2eeError::Corrupt);
    }
    const std::uint64_t remaining = *counted - 1;
    return FetchedKeyPackage{.package = std::move(*package),
                             .replenish = remaining <= core::ports::kKeyPackageLowWater};
}

class FetchKeyPackage final : public Operation {
public:
    FetchKeyPackage(const core::UserId& user, const core::DeviceId& device,
                    E2eeCallback<FetchedKeyPackage> done) noexcept
        : user_(user), device_(device), done_(std::move(done)) {}

    [[nodiscard]] Statement start() noexcept override {
        return Statement{.sql = kFetchPackage, .params = device_params(device_, user_)};
    }

    [[nodiscard]] std::optional<Statement> next(Outcome outcome) noexcept override {
        done_(outcome ? decode_fetch(*outcome) : failure<FetchedKeyPackage>(outcome.error()));
        return std::nullopt;
    }

    void abandon(DbError error) noexcept override { done_(failure<FetchedKeyPackage>(error)); }

private:
    core::UserId user_;
    core::DeviceId device_;
    E2eeCallback<FetchedKeyPackage> done_;
};

class ClaimEpoch final : public Operation {
public:
    ClaimEpoch(const core::RoomId& room, const core::UserId& user, const core::DeviceId& device,
               std::int64_t epoch, core::ports::CommitBytes commit,
               E2eeCallback<void> done) noexcept
        : room_(room), user_(user), device_(device), epoch_(epoch), commit_(std::move(commit)),
          done_(std::move(done)) {}

    [[nodiscard]] Statement start() noexcept override {
        return Statement{.sql = kClaimEpoch,
                         .params = Params{}
                                       .add_uuid(room_.uuid())
                                       .add_int(epoch_)
                                       .add_uuid(device_.uuid())
                                       .add_text(user_.view())
                                       .add_bytea(commit_)};
    }

    [[nodiscard]] std::optional<Statement> next(Outcome outcome) noexcept override {
        done_(outcome ? decode(*outcome) : failure<void>(outcome.error()));
        return std::nullopt;
    }

    void abandon(DbError error) noexcept override { done_(failure<void>(error)); }

private:
    static E2eeResult<void> decode(const Result& row) {
        if (row.rows() == 0) {
            return std::unexpected(E2eeError::NotFound);
        }
        const auto revoked = row.get(0, 0).and_then(parse_bool);
        const auto claimed = row.get(0, 1).and_then(parse_uint64);
        if (!revoked || !claimed) {
            return std::unexpected(E2eeError::Corrupt);
        }
        if (*revoked) {
            return std::unexpected(E2eeError::Revoked);
        }
        if (*claimed != 1) {
            return std::unexpected(E2eeError::StaleEpoch);
        }
        return {};
    }

    core::RoomId room_;
    core::UserId user_;
    core::DeviceId device_;
    std::int64_t epoch_;
    core::ports::CommitBytes commit_;
    E2eeCallback<void> done_;
};

E2eeResult<std::vector<StoredCommit>> decode_commits(const Result& rows) {
    std::vector<StoredCommit> page;
    page.reserve(static_cast<std::size_t>(rows.rows()));
    for (int i = 0; i < rows.rows(); ++i) {
        const auto epoch = rows.get(i, 0).and_then(parse_uint64);
        const auto body = rows.get(i, 1);
        auto commit = body ? parse_bytea(*body) : std::nullopt;
        if (!epoch || !commit) {
            return std::unexpected(E2eeError::Corrupt);
        }
        page.push_back(StoredCommit{.epoch = *epoch, .commit = std::move(*commit)});
    }
    return page;
}

} // namespace

class PgE2eeDirectory::Impl {
public:
    Impl(net::IReactor& reactor, std::unique_ptr<Pool> pool)
        : pool_(std::move(pool)), deferred_(reactor) {}

    Pool& pool() noexcept { return *pool_; }

    template <class T> void refuse(E2eeCallback<T> done, E2eeError error) {
        deferred_.post(
            [done = std::move(done), error]() mutable noexcept { done(std::unexpected(error)); });
    }

private:
    std::unique_ptr<Pool> pool_;
    DeferredCalls deferred_;
};

std::expected<std::unique_ptr<PgE2eeDirectory>, std::string>
PgE2eeDirectory::create(net::IReactor& reactor, net::OffloadPool& offload,
                        const E2eeDirectoryConfig& config) {
    // The upper bound only keeps a typo from taking the server's default max_connections (100).
    auto pool =
        Pool::create(reactor, offload,
                     PoolConfig{.conninfo = config.conninfo,
                                .application_name = "ulw-e2ee",
                                .connections = std::clamp<std::size_t>(config.connections, 1, 32),
                                .connect_timeout = config.connect_timeout,
                                .request_timeout = config.request_timeout});
    if (!pool) {
        return std::unexpected(std::move(pool.error()));
    }
    return std::make_unique<PgE2eeDirectory>(Token{},
                                             std::make_unique<Impl>(reactor, std::move(*pool)));
}

PgE2eeDirectory::PgE2eeDirectory(Token /*token*/, std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

PgE2eeDirectory::~PgE2eeDirectory() = default;

void PgE2eeDirectory::register_device(const core::UserId& user, const core::DeviceId& device,
                                      E2eeCallback<void> done) {
    impl_->pool().submit(std::make_unique<RegisterDevice>(user, device, std::move(done)));
}

void PgE2eeDirectory::deregister_device(const core::UserId& user, const core::DeviceId& device,
                                        E2eeCallback<void> done) {
    impl_->pool().submit(std::make_unique<DeregisterDevice>(user, device, std::move(done)));
}

void PgE2eeDirectory::publish_key_packages(const core::UserId& user, const core::DeviceId& device,
                                           std::vector<KeyPackageBytes> batch,
                                           E2eeCallback<std::size_t> done) {
    if (auto checked = core::ports::check_key_package_batch(batch); !checked) {
        impl_->refuse(std::move(done), checked.error());
        return;
    }
    impl_->pool().submit(std::make_unique<PublishKeyPackages>(
        user, device, encode_bytea_array(batch), std::move(done)));
}

void PgE2eeDirectory::fetch_key_package(const core::UserId& user, const core::DeviceId& device,
                                        E2eeCallback<FetchedKeyPackage> done) {
    impl_->pool().submit(std::make_unique<FetchKeyPackage>(user, device, std::move(done)));
}

void PgE2eeDirectory::submit_commit(const core::RoomId& room, const core::UserId& user,
                                    const core::DeviceId& committer, std::uint64_t epoch,
                                    core::ports::CommitBytes commit, E2eeCallback<void> done) {
    if (commit.empty() || commit.size() > core::ports::kMaxCommitBytes) {
        impl_->refuse(std::move(done), E2eeError::Invalid);
        return;
    }
    // An MLS epoch counts commits; no group comes near 2^63 of them.
    if (epoch > kMaxEpoch) {
        impl_->refuse(std::move(done), E2eeError::StaleEpoch);
        return;
    }
    impl_->pool().submit(std::make_unique<ClaimEpoch>(room, user, committer,
                                                      static_cast<std::int64_t>(epoch),
                                                      std::move(commit), std::move(done)));
}

void PgE2eeDirectory::fetch_commits(const core::RoomId& room, std::uint64_t from_epoch,
                                    E2eeCallback<std::vector<StoredCommit>> done) {
    impl_->pool().submit(std::make_unique<Query>(
        Statement{.sql = kFetchCommits,
                  .params = Params{}
                                .add_uuid(room.uuid())
                                .add_int(static_cast<std::int64_t>(std::min(from_epoch, kMaxEpoch)))
                                .add_int(static_cast<std::int64_t>(core::ports::kCommitPage))},
        [done = std::move(done)](Outcome outcome) mutable noexcept {
            done(outcome ? decode_commits(*outcome)
                         : failure<std::vector<StoredCommit>>(outcome.error()));
        }));
}

} // namespace infra::postgres
