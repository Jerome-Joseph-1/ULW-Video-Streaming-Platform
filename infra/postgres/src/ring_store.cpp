#include "infra/postgres/ring_store.hpp"

#include "operation.hpp"
#include "pool.hpp"
#include "result.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace infra::postgres {

namespace {

using core::ports::RingStoreCallback;
using core::ports::RingStoreError;
using core::ports::RingStoreResult;
using core::ports::StoredRing;

// The room rings this call now, whatever it rang before. $6 and $7 in microseconds.
constexpr Sql kPut = R"sql(
INSERT INTO chat_call_rings (room_id, call_id, caller, is_group, callees, started_at, expires_at)
VALUES ($1, $2, $3, $4, string_to_array($5, ' '),
        timestamptz 'epoch' + $6 * interval '1 microsecond',
        timestamptz 'epoch' + $7 * interval '1 microsecond')
ON CONFLICT (room_id) DO UPDATE
   SET call_id = excluded.call_id, caller = excluded.caller, is_group = excluded.is_group,
       callees = excluded.callees, started_at = excluded.started_at,
       expires_at = excluded.expires_at)sql";

// The room's ring ended, if it is still this call's; and any ring an hour past its end, which
// nothing else would ever delete.
constexpr Sql kEnd = R"sql(
DELETE FROM chat_call_rings
 WHERE (room_id = $1 AND call_id = $2) OR expires_at < now() - interval '1 hour')sql";

constexpr Sql kRingOf = R"sql(
SELECT room_id, call_id, caller, is_group, array_to_string(callees, ' '),
       (extract(epoch FROM started_at) * 1000000)::bigint,
       (extract(epoch FROM expires_at) * 1000000)::bigint
  FROM chat_call_rings
 WHERE room_id = $1 AND expires_at > now())sql";

constexpr Sql kRingsFor = R"sql(
SELECT room_id, call_id, caller, is_group, array_to_string(callees, ' '),
       (extract(epoch FROM started_at) * 1000000)::bigint,
       (extract(epoch FROM expires_at) * 1000000)::bigint
  FROM chat_call_rings
 WHERE callees @> ARRAY[$1::text] AND expires_at > now()
 ORDER BY started_at DESC
 LIMIT $2)sql";

std::int64_t micros(core::WallTime t) noexcept {
    return std::chrono::duration_cast<std::chrono::microseconds>(t.time_since_epoch()).count();
}

// One statement whose parameters it owns: texts outlive the Params that borrow them, across
// reruns.
class Bound final : public Operation {
public:
    using Done = std::move_only_function<void(Outcome) noexcept>;
    using Arg = std::variant<std::int64_t, bool, std::string, core::Uuid>;

    Bound(Sql sql, std::vector<Arg> args, Done done) noexcept
        : sql_(sql), args_(std::move(args)), done_(std::move(done)) {}

    [[nodiscard]] Statement start() noexcept override {
        Params params;
        for (const Arg& arg : args_) {
            if (const auto* i = std::get_if<std::int64_t>(&arg)) {
                params.add_int(*i);
            } else if (const auto* b = std::get_if<bool>(&arg)) {
                params.add_bool(*b);
            } else if (const auto* t = std::get_if<std::string>(&arg)) {
                params.add_text(*t);
            } else {
                params.add_uuid(std::get<core::Uuid>(arg));
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

RingStoreResult<std::vector<StoredRing>> decode_rings(const Result& rows) {
    std::vector<StoredRing> out;
    out.reserve(static_cast<std::size_t>(rows.rows()));
    for (int i = 0; i < rows.rows(); ++i) {
        const auto room = domain_at<core::RoomId>(rows, i, 0);
        const auto call =
            rows.get(i, 1).and_then([](std::string_view t) { return core::Uuid::parse(t); });
        const auto caller = domain_at<core::UserId>(rows, i, 2);
        const auto group = rows.get(i, 3).and_then(parse_bool);
        const auto callees = rows.get(i, 4);
        const auto started = rows.get(i, 5).and_then(parse_int64);
        const auto expires = rows.get(i, 6).and_then(parse_int64);
        if (!room || !call || !caller || !group || !callees || !started || !expires) {
            return std::unexpected(RingStoreError::Corrupt);
        }
        StoredRing ring{.room = *room,
                        .call = *call,
                        .caller = *caller,
                        .group = *group,
                        .callees = {},
                        .started_at = core::WallTime{std::chrono::microseconds{*started}},
                        .expires_at = core::WallTime{std::chrono::microseconds{*expires}}};
        std::string_view rest = *callees;
        while (!rest.empty()) {
            const std::size_t space = rest.find(' ');
            const auto user = core::UserId::parse(rest.substr(0, space));
            if (!user) {
                return std::unexpected(RingStoreError::Corrupt);
            }
            ring.callees.push_back(*user);
            rest = space == std::string_view::npos ? std::string_view{} : rest.substr(space + 1);
        }
        out.push_back(std::move(ring));
    }
    return out;
}

} // namespace

class PgRingStore::Impl {
public:
    explicit Impl(std::unique_ptr<Pool> pool) : pool_(std::move(pool)) {}
    Pool& pool() noexcept { return *pool_; }

private:
    std::unique_ptr<Pool> pool_;
};

std::expected<std::unique_ptr<PgRingStore>, std::string>
PgRingStore::create(net::IReactor& reactor, net::OffloadPool& offload,
                    const RingStoreConfig& config) {
    auto pool =
        Pool::create(reactor, offload,
                     PoolConfig{.conninfo = config.conninfo,
                                .application_name = "ulw-rings",
                                .connections = std::clamp<std::size_t>(config.connections, 1, 8),
                                .connect_timeout = config.connect_timeout,
                                .request_timeout = config.request_timeout});
    if (!pool) {
        return std::unexpected(std::move(pool.error()));
    }
    return std::make_unique<PgRingStore>(Token{}, std::make_unique<Impl>(std::move(*pool)));
}

PgRingStore::PgRingStore(Token /*token*/, std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

PgRingStore::~PgRingStore() = default;

void PgRingStore::put_ring(StoredRing ring, RingStoreCallback<void> done) {
    std::string callees;
    for (std::size_t i = 0; i < ring.callees.size() && i < core::ports::kMaxStoredCallees; ++i) {
        if (i != 0) {
            callees += ' ';
        }
        callees += ring.callees[i].view();
    }
    impl_->pool().submit(std::make_unique<Bound>(
        kPut,
        std::vector<Bound::Arg>{ring.room.uuid(), ring.call, std::string(ring.caller.view()),
                                ring.group, std::move(callees), micros(ring.started_at),
                                micros(ring.expires_at)},
        [done = std::move(done)](Outcome outcome) mutable noexcept {
            done(outcome ? RingStoreResult<void>{} : std::unexpected(RingStoreError::Unavailable));
        }));
}

void PgRingStore::end_ring(const core::RoomId& room, const core::Uuid& call,
                           RingStoreCallback<void> done) {
    impl_->pool().submit(std::make_unique<Bound>(
        kEnd, std::vector<Bound::Arg>{room.uuid(), call},
        [done = std::move(done)](Outcome outcome) mutable noexcept {
            done(outcome ? RingStoreResult<void>{} : std::unexpected(RingStoreError::Unavailable));
        }));
}

void PgRingStore::ring_of(const core::RoomId& room,
                          RingStoreCallback<std::optional<StoredRing>> done) {
    impl_->pool().submit(std::make_unique<Bound>(
        kRingOf, std::vector<Bound::Arg>{room.uuid()},
        [done = std::move(done)](Outcome outcome) mutable noexcept {
            if (!outcome) {
                done(std::unexpected(RingStoreError::Unavailable));
                return;
            }
            auto rings = decode_rings(*outcome);
            if (!rings) {
                done(std::unexpected(rings.error()));
                return;
            }
            done(rings->empty() ? std::optional<StoredRing>{}
                                : std::optional<StoredRing>(std::move(rings->front())));
        }));
}

void PgRingStore::rings_for(const core::UserId& user,
                            RingStoreCallback<std::vector<StoredRing>> done) {
    impl_->pool().submit(std::make_unique<Bound>(
        kRingsFor,
        std::vector<Bound::Arg>{std::string(user.view()),
                                static_cast<std::int64_t>(core::ports::kMaxRingsForUser)},
        [done = std::move(done)](Outcome outcome) mutable noexcept {
            done(outcome ? decode_rings(*outcome) : std::unexpected(RingStoreError::Unavailable));
        }));
}

} // namespace infra::postgres
