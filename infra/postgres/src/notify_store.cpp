#include "infra/postgres/notify_store.hpp"

#include "deferred_calls.hpp"
#include "operation.hpp"
#include "pool.hpp"

#include <algorithm>
#include <bit>
#include <optional>
#include <string>
#include <utility>
#include <variant>

namespace infra::postgres {

namespace {

using core::ports::NotifyCallback;
using core::ports::NotifyError;
using core::ports::NotifyResult;
using core::ports::SocketDevice;

// The nil UUID stands for a socket that named no device: the key needs a value.
constexpr std::string_view kNoDevice = "00000000-0000-0000-0000-000000000000";

// Renews the node's lease, and forgets the nodes (with their sockets) and the marks nobody needs
// any more. Answers whether the node's row was made now rather than renewed: a node forgotten
// meanwhile (its renewals failed for forget_after) has lost its sockets, and writes them again.
constexpr Sql kRenew = R"sql(
WITH renewed AS (
    INSERT INTO notify_nodes (node_tag) VALUES ($1)
    ON CONFLICT (node_tag) DO UPDATE SET renewed_at = now()
    RETURNING xmax = 0 AS made),
gone AS (
    DELETE FROM notify_nodes
     WHERE renewed_at < now() - $2::float8 * interval '1 millisecond' AND node_tag <> $1),
stale AS (
    DELETE FROM notify_marks WHERE told_at < now() - $3::float8 * interval '1 millisecond')
SELECT made FROM renewed)sql";

// The node's changes, as one statement: both lists are sets, so it can be sent again whole if
// its answer was lost. A user who connects has their marks cleared, so their next absence
// starts afresh. $2/$3 the removed users and devices, $4/$5 the added, each space-separated
// (neither a user id nor a UUID holds a space).
constexpr Sql kSync = R"sql(
WITH removed AS (
    DELETE FROM notify_sockets s
     USING unnest(string_to_array($2, ' '), string_to_array($3, ' ')::uuid[]) AS r (u, d)
     WHERE s.node_tag = $1 AND s.user_id = r.u AND s.device_id = r.d),
added AS (
    INSERT INTO notify_sockets (node_tag, user_id, device_id)
    SELECT $1, u, d
      FROM unnest(string_to_array($4, ' '), string_to_array($5, ' ')::uuid[]) AS a (u, d)
    ON CONFLICT DO NOTHING),
cleared AS (
    DELETE FROM notify_marks WHERE user_id = ANY (string_to_array($4, ' ')))
SELECT 1)sql";

constexpr Sql kForget = "DELETE FROM notify_nodes WHERE node_tag = $1";

constexpr Sql kConnected = R"sql(
SELECT DISTINCT s.user_id, s.device_id
  FROM notify_sockets s JOIN notify_nodes n USING (node_tag)
 WHERE s.user_id = ANY (string_to_array($1, ' '))
   AND n.renewed_at >= now() - $2::float8 * interval '1 millisecond'
 ORDER BY 1, 2)sql";

// Who is owed a notification of a message in the room, marked told in the same statement. The
// conflict update waits on a mark another node is writing, then sees its told_at, so a member is
// answered once however many nodes ask at once. $1 the room, $2 the sender, $3 the lease and $4
// the interval in milliseconds.
constexpr Sql kTargets = R"sql(
WITH due AS (
    SELECT m.user_id FROM chat_members m
      JOIN chat_rooms r ON r.room_id = m.room_id AND r.kind IN ('direct_chat', 'group_chat')
     WHERE m.room_id = $1 AND m.user_id <> $2
       AND NOT EXISTS (
           SELECT 1 FROM notify_sockets s JOIN notify_nodes n USING (node_tag)
            WHERE s.user_id = m.user_id
              AND n.renewed_at >= now() - $3::float8 * interval '1 millisecond'))
INSERT INTO notify_marks (room_id, user_id, told_at)
SELECT $1, user_id, now() FROM due
ON CONFLICT (room_id, user_id) DO UPDATE SET told_at = excluded.told_at
 WHERE notify_marks.told_at <= now() - $4::float8 * interval '1 millisecond'
RETURNING user_id)sql";

// The oldest changes nobody holds, taken for good. RETURNING has no order: decode sorts by id.
constexpr Sql kTakeVideoEvents = R"sql(
DELETE FROM video_events
 WHERE id IN (SELECT id FROM video_events ORDER BY id LIMIT $1 FOR UPDATE SKIP LOCKED)
RETURNING id, video_id, owner_id, state, version, duration_ms, error_reason,
          (extract(epoch FROM at) * 1000)::bigint)sql";

NotifyError to_notify_error(DbError /*e*/) noexcept {
    // A constraint is a sync after the node was forgotten (its row gone): renewing writes it
    // again, which is what an unavailable store asks of the caller too.
    return NotifyError::Unavailable;
}

std::int64_t tag_param(std::uint64_t node) noexcept {
    return std::bit_cast<std::int64_t>(node);
}

std::int64_t millis(core::Millis m) noexcept {
    return m.count();
}

// One statement whose parameters it owns: texts outlive the Params that borrow them, across
// reruns.
class Bound final : public Operation {
public:
    using Done = std::move_only_function<void(Outcome) noexcept>;
    using Arg = std::variant<std::int64_t, std::string, core::RoomId>;

    Bound(Sql sql, std::vector<Arg> args, Done done) noexcept
        : sql_(sql), args_(std::move(args)), done_(std::move(done)) {}

    [[nodiscard]] Statement start() noexcept override {
        Params params;
        for (const Arg& arg : args_) {
            if (const auto* i = std::get_if<std::int64_t>(&arg)) {
                params.add_int(*i);
            } else if (const auto* t = std::get_if<std::string>(&arg)) {
                params.add_text(*t);
            } else {
                params.add_uuid(std::get<core::RoomId>(arg).uuid());
            }
        }
        return Statement{.sql = sql_, .params = params};
    }

    [[nodiscard]] std::optional<Statement> next(Outcome outcome) noexcept override {
        done_(std::move(outcome));
        return std::nullopt;
    }

    void abandon(DbError error) noexcept override { done_(std::unexpected(error)); }

private:
    Sql sql_;
    std::vector<Arg> args_;
    Done done_;
};

// Space-separated user ids and devices, in the same order.
struct Columns {
    std::string users;
    std::string devices;
};

Columns columns_of(const std::vector<SocketDevice>& sockets) {
    Columns out;
    for (const SocketDevice& s : sockets) {
        if (!out.users.empty()) {
            out.users += ' ';
            out.devices += ' ';
        }
        out.users += s.user.view();
        out.devices += s.device ? s.device->to_string() : std::string(kNoDevice);
    }
    return out;
}

std::string users_of(const std::vector<core::UserId>& users) {
    std::string out;
    for (const core::UserId& u : users) {
        if (!out.empty()) {
            out += ' ';
        }
        out += u.view();
    }
    return out;
}

NotifyResult<std::vector<SocketDevice>> decode_sockets(const Result& rows) {
    std::vector<SocketDevice> out;
    out.reserve(static_cast<std::size_t>(rows.rows()));
    for (int i = 0; i < rows.rows(); ++i) {
        const auto user = rows.get(i, 0);
        const auto device = rows.get(i, 1);
        if (!user || !device) {
            return std::unexpected(NotifyError::Corrupt);
        }
        auto parsed_user = core::UserId::parse(*user);
        if (!parsed_user) {
            return std::unexpected(NotifyError::Corrupt);
        }
        std::optional<core::DeviceId> named;
        if (*device != kNoDevice) {
            auto parsed = core::DeviceId::parse(*device);
            if (!parsed) {
                return std::unexpected(NotifyError::Corrupt);
            }
            named = *parsed;
        }
        out.push_back(SocketDevice{.user = *parsed_user, .device = named});
    }
    return out;
}

NotifyResult<std::vector<core::UserId>> decode_users(const Result& rows) {
    std::vector<core::UserId> out;
    out.reserve(static_cast<std::size_t>(rows.rows()));
    for (int i = 0; i < rows.rows(); ++i) {
        const auto user = rows.get(i, 0);
        if (!user) {
            return std::unexpected(NotifyError::Corrupt);
        }
        auto parsed = core::UserId::parse(*user);
        if (!parsed) {
            return std::unexpected(NotifyError::Corrupt);
        }
        out.push_back(*parsed);
    }
    std::ranges::sort(out, {}, &core::UserId::view);
    return out;
}

NotifyResult<std::vector<core::ports::VideoEvent>> decode_video_events(const Result& rows) {
    std::vector<std::pair<std::int64_t, core::ports::VideoEvent>> taken;
    taken.reserve(static_cast<std::size_t>(rows.rows()));
    for (int i = 0; i < rows.rows(); ++i) {
        const auto id = rows.get(i, 0).and_then(parse_int64);
        const auto video =
            rows.get(i, 1).transform([](std::string_view t) { return core::VideoId::parse(t); });
        const auto owner =
            rows.get(i, 2).transform([](std::string_view t) { return core::UserId::parse(t); });
        const auto state = rows.get(i, 3);
        const auto version = rows.get(i, 4).and_then(parse_int64);
        const auto at = rows.get(i, 7).and_then(parse_int64);
        if (!id || !video || !*video || !owner || !*owner || !state || !version || !at ||
            (*state != "ready" && *state != "failed")) {
            return std::unexpected(NotifyError::Corrupt);
        }
        core::ports::VideoEvent event{.video = **video,
                                      .owner = **owner,
                                      .ready = *state == "ready",
                                      .version = static_cast<std::int32_t>(*version),
                                      .duration = std::nullopt,
                                      .error_reason = std::nullopt,
                                      .at_ms = *at};
        if (const auto duration = rows.get(i, 5)) {
            const auto ms = parse_int64(*duration);
            if (!ms) {
                return std::unexpected(NotifyError::Corrupt);
            }
            event.duration = core::Millis{*ms};
        }
        if (const auto reason = rows.get(i, 6)) {
            event.error_reason = std::string(*reason);
        }
        taken.emplace_back(*id, std::move(event));
    }
    std::ranges::sort(taken, {}, &std::pair<std::int64_t, core::ports::VideoEvent>::first);
    std::vector<core::ports::VideoEvent> out;
    out.reserve(taken.size());
    for (auto& [id, event] : taken) {
        out.push_back(std::move(event));
    }
    return out;
}

} // namespace

class PgNotifyStore::Impl {
public:
    Impl(net::IReactor& reactor, std::unique_ptr<Pool> pool, core::ports::NotifyTiming timing)
        : pool_(std::move(pool)), deferred_(reactor), timing_(timing) {}

    Pool& pool() noexcept { return *pool_; }
    [[nodiscard]] const core::ports::NotifyTiming& timing() const noexcept { return timing_; }

    template <class T> void refuse(NotifyCallback<T> done) {
        deferred_.post([done = std::move(done)]() mutable noexcept {
            done(std::unexpected(NotifyError::Unavailable));
        });
    }

private:
    std::unique_ptr<Pool> pool_;
    DeferredCalls deferred_;
    core::ports::NotifyTiming timing_;
};

std::expected<std::unique_ptr<PgNotifyStore>, std::string>
PgNotifyStore::create(net::IReactor& reactor, net::OffloadPool& offload,
                      const NotifyStoreConfig& config) {
    auto pool =
        Pool::create(reactor, offload,
                     PoolConfig{.conninfo = config.conninfo,
                                .application_name = "ulw-notify",
                                .connections = std::clamp<std::size_t>(config.connections, 1, 8),
                                .connect_timeout = config.connect_timeout,
                                .request_timeout = config.request_timeout});
    if (!pool) {
        return std::unexpected(std::move(pool.error()));
    }
    return std::make_unique<PgNotifyStore>(
        Token{}, std::make_unique<Impl>(reactor, std::move(*pool), config.timing));
}

PgNotifyStore::PgNotifyStore(Token /*token*/, std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

PgNotifyStore::~PgNotifyStore() = default;

void PgNotifyStore::renew(std::uint64_t node, NotifyCallback<bool> done) {
    impl_->pool().submit(std::make_unique<Bound>(
        kRenew,
        std::vector<Bound::Arg>{tag_param(node), millis(impl_->timing().forget_after),
                                millis(impl_->timing().message_interval)},
        [done = std::move(done)](Outcome outcome) mutable noexcept {
            if (!outcome) {
                done(std::unexpected(to_notify_error(outcome.error())));
                return;
            }
            const auto made =
                outcome->rows() == 1 ? outcome->get(0, 0).and_then(parse_bool) : std::nullopt;
            if (!made) {
                done(std::unexpected(NotifyError::Corrupt));
                return;
            }
            done(*made);
        }));
}

void PgNotifyStore::sync(std::uint64_t node, std::vector<SocketDevice> added,
                         std::vector<SocketDevice> removed, NotifyCallback<void> done) {
    const bool overlap = std::ranges::any_of(added, [&](const SocketDevice& a) {
        return std::ranges::find(removed, a) != removed.end();
    });
    if (overlap || added.size() + removed.size() > core::ports::kMaxSyncRows) {
        impl_->refuse(std::move(done));
        return;
    }
    Columns gone = columns_of(removed);
    Columns come = columns_of(added);
    impl_->pool().submit(std::make_unique<Bound>(
        kSync,
        std::vector<Bound::Arg>{tag_param(node), std::move(gone.users), std::move(gone.devices),
                                std::move(come.users), std::move(come.devices)},
        [done = std::move(done)](Outcome outcome) mutable noexcept {
            if (!outcome) {
                done(std::unexpected(to_notify_error(outcome.error())));
                return;
            }
            done({});
        }));
}

void PgNotifyStore::forget(std::uint64_t node, NotifyCallback<void> done) {
    impl_->pool().submit(
        std::make_unique<Bound>(kForget, std::vector<Bound::Arg>{tag_param(node)},
                                [done = std::move(done)](Outcome outcome) mutable noexcept {
                                    if (!outcome) {
                                        done(std::unexpected(to_notify_error(outcome.error())));
                                        return;
                                    }
                                    done({});
                                }));
}

void PgNotifyStore::connected(std::vector<core::UserId> users,
                              NotifyCallback<std::vector<SocketDevice>> done) {
    if (users.size() > core::ports::kMaxConnectedQuery) {
        impl_->refuse(std::move(done));
        return;
    }
    impl_->pool().submit(std::make_unique<Bound>(
        kConnected, std::vector<Bound::Arg>{users_of(users), millis(impl_->timing().lease)},
        [done = std::move(done)](Outcome outcome) mutable noexcept {
            done(outcome ? decode_sockets(*outcome)
                         : std::unexpected(to_notify_error(outcome.error())));
        }));
}

void PgNotifyStore::message_targets(const core::RoomId& room, const core::UserId& sender,
                                    NotifyCallback<std::vector<core::UserId>> done) {
    impl_->pool().submit(std::make_unique<Bound>(
        kTargets,
        std::vector<Bound::Arg>{room, std::string(sender.view()), millis(impl_->timing().lease),
                                millis(impl_->timing().message_interval)},
        [done = std::move(done)](Outcome outcome) mutable noexcept {
            done(outcome ? decode_users(*outcome)
                         : std::unexpected(to_notify_error(outcome.error())));
        }));
}

void PgNotifyStore::take_video_events(std::size_t max,
                                      NotifyCallback<std::vector<core::ports::VideoEvent>> done) {
    if (max == 0 || max > core::ports::kMaxVideoEvents) {
        impl_->refuse(std::move(done));
        return;
    }
    impl_->pool().submit(std::make_unique<Bound>(
        kTakeVideoEvents, std::vector<Bound::Arg>{static_cast<std::int64_t>(max)},
        [done = std::move(done)](Outcome outcome) mutable noexcept {
            done(outcome ? decode_video_events(*outcome)
                         : std::unexpected(to_notify_error(outcome.error())));
        }));
}

} // namespace infra::postgres
