#include "infra/postgres/room_store.hpp"

#include "core/ports/message_store.hpp"

#include "operation.hpp"
#include "pool.hpp"
#include "result.hpp"
#include "timer.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace infra::postgres {

namespace {

using rt::OwnedRoom;
using rt::Ownership;
using rt::StoreCallback;
using rt::StoreError;
using rt::StoreResult;

// A room nobody has asked for before is created by the first node that resolves it. Both rows
// in one statement: a room never exists without its sequence counter. Its kind is the one its
// first chat join recorded in chat_rooms, which runs before the room is resolved; a room no
// chat join recorded takes $3 (kind_of_unrecorded). A live chat is delivered lossy.
constexpr Sql kCreateRoom = R"sql(
WITH created AS (
    INSERT INTO room_assignments (room_id, owner_node) VALUES ($1, $2)
    ON CONFLICT (room_id) DO NOTHING
    RETURNING owner_generation),
kind AS (
    SELECT coalesce((SELECT kind FROM chat_rooms WHERE room_id = $1), $3) AS kind)
INSERT INTO room_state (room_id, owner_generation, kind, delivery)
SELECT $1, created.owner_generation, kind.kind,
       CASE WHEN kind.kind = 'stream_live_chat' THEN 'lossy' ELSE 'durable' END
  FROM created, kind
RETURNING owner_generation)sql";

// The kind of a room created without a chat join recording one first: closed, so that no room
// is open by default. The one place a room's kind is chosen from the room alone.
[[nodiscard]] std::string_view kind_of_unrecorded(const core::RoomId& /*room*/) noexcept {
    return "group_chat";
}

// The fence moves with the owner: room_state takes the new generation in the same statement,
// so from its commit on only the new owner's appends match. Under READ COMMITTED a second
// claimant blocks on the row lock, then re-reads a fresh heartbeat and updates nothing.
// A room recorded as the claimant's own is taken again only if its heartbeat predates the
// claimant's latest advertise, i.e. an earlier run of the node wrote it. A claim this very run
// made moments ago (its sweep racing this lookup) is newer, and is kept, not fenced out; a node
// that has not advertised yet is treated as a fresh run.
constexpr Sql kClaimRoom = R"sql(
WITH claimed AS (
    UPDATE room_assignments
       SET owner_node = $2, owner_generation = owner_generation + 1, heartbeat_at = now()
     WHERE room_id = $1
       AND ((owner_node = $2
             AND heartbeat_at < coalesce((SELECT started_at FROM chat_nodes WHERE node_id = $2),
                                         'infinity'))
            OR heartbeat_at < now() - $3 * interval '1 millisecond')
    RETURNING owner_generation)
UPDATE room_state SET owner_generation = claimed.owner_generation
  FROM claimed WHERE room_state.room_id = $1
RETURNING room_state.owner_generation)sql";

constexpr Sql kReadOwner =
    "SELECT owner_node, owner_generation FROM room_assignments WHERE room_id = $1";

// Any number of rooms is one statement.
constexpr Sql kClaimStale = R"sql(
WITH claimed AS (
    UPDATE room_assignments
       SET owner_node = $1, owner_generation = owner_generation + 1, heartbeat_at = now()
     WHERE room_id = ANY ($2::text::uuid[])
       AND heartbeat_at < now() - $3 * interval '1 millisecond'
    RETURNING room_id, owner_generation)
UPDATE room_state SET owner_generation = claimed.owner_generation
  FROM claimed WHERE room_state.room_id = claimed.room_id
RETURNING room_state.room_id, room_state.owner_generation)sql";

// Owner writes: each matches a room only under the generation its writer holds.
constexpr Sql kHeartbeat = R"sql(
WITH seen AS (
    UPDATE chat_nodes SET seen_at = now() WHERE node_id = $1 AND incarnation = $4)
UPDATE room_assignments SET heartbeat_at = now()
  FROM unnest($2::text::uuid[], $3::text::bigint[]) AS held (room_id, generation)
 WHERE room_assignments.room_id = held.room_id
   AND room_assignments.owner_generation = held.generation
   AND room_assignments.owner_node = $1
RETURNING room_assignments.room_id)sql";

// The fenced append and the message's row in one statement, so one transaction and one commit:
// a seq is taken only with its row, and a fenced writer takes neither. A message whose key the
// sender already used in the room takes no seq: with the same body it is a repeat, answered
// with the seq it was stored under; with another body it is a conflict, answered with none.
// The room_state row is updated (by nothing) either way, so that those answers are fenced too.
// A concurrent repeat of the same key that commits first makes this one fail on the key's
// unique index, whole, seq included; run again, it finds the stored row. A lossy room (a
// stream's live chat) keeps its newest 1000: storing seq N deletes seq N - 1000, one more
// primary key write, so a live room's rows stay about 2 MiB however long the stream (ADR-0057).
constexpr Sql kAppendMessage = R"sql(
WITH prior AS (
    SELECT seq, body = $5 AS same
      FROM chat_messages WHERE room_id = $1 AND sender = $3 AND msg_key = $4),
next AS (
    UPDATE room_state
       SET last_seq = last_seq + CASE WHEN EXISTS (SELECT 1 FROM prior) THEN 0 ELSE 1 END
     WHERE room_id = $1 AND owner_generation = $2
    RETURNING last_seq, delivery),
stored AS (
    INSERT INTO chat_messages (room_id, seq, sender, msg_key, body, sent_at)
    SELECT $1, last_seq, $3, $4, $5, now()
      FROM next
     WHERE NOT EXISTS (SELECT 1 FROM prior)
    RETURNING seq),
pruned AS (
    DELETE FROM chat_messages
     WHERE room_id = $1
       AND seq = (SELECT last_seq - 1000 FROM next WHERE delivery = 'lossy')
       AND EXISTS (SELECT 1 FROM stored))
SELECT seq, same FROM prior WHERE EXISTS (SELECT 1 FROM next)
UNION ALL
SELECT seq, true FROM stored)sql";

// '-infinity' is older than any staleness bound, so the rooms are claimable at once.
constexpr Sql kRelease = R"sql(
UPDATE room_assignments SET heartbeat_at = '-infinity'
  FROM unnest($2::text::uuid[], $3::text::bigint[]) AS held (room_id, generation)
 WHERE room_assignments.room_id = held.room_id
   AND room_assignments.owner_generation = held.generation
   AND room_assignments.owner_node = $1)sql";

// Another incarnation's hold ends kOwnerStaleAfter after its last heartbeat, the same bound
// its rooms go stale by. Re-advertising the same incarnation keeps its start.
constexpr Sql kAdvertise = R"sql(
INSERT INTO chat_nodes (node_id, address, incarnation) VALUES ($1, $2, $3)
ON CONFLICT (node_id) DO UPDATE
   SET address = excluded.address, incarnation = excluded.incarnation,
       started_at = CASE WHEN chat_nodes.incarnation = excluded.incarnation
                         THEN chat_nodes.started_at ELSE now() END,
       seen_at = now()
 WHERE chat_nodes.incarnation = excluded.incarnation
    OR chat_nodes.seen_at < now() - $4 * interval '1 millisecond')sql";

constexpr Sql kReadOwners = R"sql(
SELECT room_id, owner_node, owner_generation FROM room_assignments
 WHERE room_id = ANY ($1::text::uuid[]))sql";

constexpr Sql kFindAddress = "SELECT address FROM chat_nodes WHERE node_id = $1";

constexpr Sql kListen = "LISTEN room_owner";

std::int64_t stale_after_ms() noexcept {
    return rt::kOwnerStaleAfter.count();
}

// Generations start at 1 and only ever rise, far below 2^63.
std::optional<std::uint64_t> generation_at(const Result& r, int row, int column) noexcept {
    const auto value = r.get(row, column).and_then(parse_int64);
    if (!value || *value < 1) {
        return std::nullopt;
    }
    return static_cast<std::uint64_t>(*value);
}

// Room lists and generations bind as array literals. Uuids and integers need no quoting.
template <class Items, class Text> std::string array_literal(const Items& items, Text text) {
    std::string out = "{";
    for (const auto& item : items) {
        if (out.size() > 1) {
            out += ',';
        }
        out += text(item);
    }
    out += '}';
    return out;
}

std::string room_of(const OwnedRoom& r) {
    return r.room.to_string();
}

std::string generation_of(const OwnedRoom& r) {
    return std::to_string(r.generation);
}

// Generations fit in bigint: they start at 1 and rise by one per claim.
std::int64_t as_int(std::uint64_t generation) noexcept {
    return static_cast<std::int64_t>(generation);
}

// Parameters are views, so every operation keeps what they point at: the pool sends the
// statement on a later iteration, and again after a serialization failure.
class Resolve final : public Operation {
public:
    Resolve(const core::RoomId& room, const core::NodeId& node, StoreCallback<Ownership> done)
        : room_(room), node_(node), done_(std::move(done)) {}

    [[nodiscard]] Statement start() noexcept override {
        step_ = Step::Create;
        return Statement{.sql = kCreateRoom,
                         .params = Params{}
                                       .add_uuid(room_.uuid())
                                       .add_text(node_.view())
                                       .add_text(kind_of_unrecorded(room_))};
    }

    [[nodiscard]] std::optional<Statement> next(Outcome outcome) noexcept override {
        if (!outcome) {
            return finish(std::unexpected(StoreError::Unavailable));
        }
        switch (step_) {
        case Step::Create:
            if (outcome->rows() == 1) {
                return mine(*outcome);
            }
            step_ = Step::Claim;
            return Statement{.sql = kClaimRoom,
                             .params = Params{}
                                           .add_uuid(room_.uuid())
                                           .add_text(node_.view())
                                           .add_int(stale_after_ms())};
        case Step::Claim:
            if (outcome->rows() == 1) {
                return mine(*outcome);
            }
            step_ = Step::Read;
            return Statement{.sql = kReadOwner, .params = Params{}.add_uuid(room_.uuid())};
        case Step::Read:
            return finish(read_owner(*outcome));
        }
        return finish(std::unexpected(StoreError::Unavailable));
    }

    void abandon(DbError /*error*/) noexcept override {
        done_(std::unexpected(StoreError::Unavailable));
    }

private:
    enum class Step : std::uint8_t { Create, Claim, Read };

    std::optional<Statement> mine(const Result& r) noexcept {
        const auto generation = generation_at(r, 0, 0);
        if (!generation) {
            return finish(std::unexpected(StoreError::Corrupt));
        }
        return finish(Ownership{.node = node_, .generation = *generation});
    }

    // Held, and not stale, when the claim ran. The room was created by then, so no row means
    // it was deleted meanwhile, which nothing here does.
    static StoreResult<Ownership> read_owner(const Result& r) noexcept {
        if (r.rows() == 0) {
            return std::unexpected(StoreError::Unavailable);
        }
        const auto node = r.get(0, 0).transform(core::NodeId::parse);
        const auto generation = generation_at(r, 0, 1);
        if (!node || !*node || !generation) {
            return std::unexpected(StoreError::Corrupt);
        }
        return Ownership{.node = **node, .generation = *generation};
    }

    std::optional<Statement> finish(StoreResult<Ownership> result) noexcept {
        done_(result);
        return std::nullopt;
    }

    core::RoomId room_;
    core::NodeId node_;
    Step step_ = Step::Create;
    StoreCallback<Ownership> done_;
};

class ClaimStale final : public Operation {
public:
    ClaimStale(const std::vector<core::RoomId>& rooms, const core::NodeId& node,
               StoreCallback<std::vector<OwnedRoom>> done)
        : node_(node),
          rooms_(array_literal(rooms, [](const core::RoomId& room) { return room.to_string(); })),
          done_(std::move(done)) {}

    [[nodiscard]] Statement start() noexcept override {
        return Statement{
            .sql = kClaimStale,
            .params = Params{}.add_text(node_.view()).add_text(rooms_).add_int(stale_after_ms())};
    }

    [[nodiscard]] std::optional<Statement> next(Outcome outcome) noexcept override {
        if (!outcome) {
            done_(std::unexpected(StoreError::Unavailable));
            return std::nullopt;
        }
        done_(decode(*outcome));
        return std::nullopt;
    }

    void abandon(DbError /*error*/) noexcept override {
        done_(std::unexpected(StoreError::Unavailable));
    }

private:
    static StoreResult<std::vector<OwnedRoom>> decode(const Result& r) {
        std::vector<OwnedRoom> claimed;
        claimed.reserve(static_cast<std::size_t>(r.rows()));
        for (int row = 0; row < r.rows(); ++row) {
            const auto room = domain_at<core::RoomId>(r, row, 0);
            const auto generation = generation_at(r, row, 1);
            if (!room || !generation) {
                return std::unexpected(StoreError::Corrupt);
            }
            claimed.push_back({.room = *room, .generation = *generation});
        }
        return claimed;
    }

    core::NodeId node_;
    std::string rooms_;
    StoreCallback<std::vector<OwnedRoom>> done_;
};

// The heartbeat and the release: the same fenced match on (room, generation, node). The
// heartbeat also binds the incarnation, as $4.
template <class T> class HeldRooms final : public Operation {
public:
    using Decode = StoreResult<T> (*)(const Result&);

    HeldRooms(Sql sql, const core::NodeId& node, std::optional<core::Uuid> incarnation,
              const std::vector<OwnedRoom>& rooms, Decode decode, StoreCallback<T> done)
        : sql_(sql), node_(node), incarnation_(incarnation), rooms_(array_literal(rooms, room_of)),
          generations_(array_literal(rooms, generation_of)), decode_(decode),
          done_(std::move(done)) {}

    [[nodiscard]] Statement start() noexcept override {
        Params params;
        params.add_text(node_.view()).add_text(rooms_).add_text(generations_);
        if (incarnation_) {
            params.add_uuid(*incarnation_);
        }
        return Statement{.sql = sql_, .params = params};
    }

    [[nodiscard]] std::optional<Statement> next(Outcome outcome) noexcept override {
        if (!outcome) {
            done_(std::unexpected(StoreError::Unavailable));
            return std::nullopt;
        }
        done_(decode_(*outcome));
        return std::nullopt;
    }

    void abandon(DbError /*error*/) noexcept override {
        done_(std::unexpected(StoreError::Unavailable));
    }

private:
    Sql sql_;
    core::NodeId node_;
    std::optional<core::Uuid> incarnation_;
    std::string rooms_;
    std::string generations_;
    Decode decode_;
    StoreCallback<T> done_;
};

StoreResult<std::vector<core::RoomId>> decode_renewed(const Result& r) {
    std::vector<core::RoomId> renewed;
    renewed.reserve(static_cast<std::size_t>(r.rows()));
    for (int row = 0; row < r.rows(); ++row) {
        const auto room = domain_at<core::RoomId>(r, row, 0);
        if (!room) {
            return std::unexpected(StoreError::Corrupt);
        }
        renewed.push_back(*room);
    }
    return renewed;
}

StoreResult<void> decode_nothing(const Result& /*r*/) {
    return {};
}

// The seq, a repeat's stored seq, or no row when fenced; a stored key with another body is a
// conflict.
[[nodiscard]] StoreResult<std::optional<std::uint64_t>> decode_append(const Result& r) noexcept {
    if (r.rows() == 0) {
        return std::optional<std::uint64_t>{};
    }
    const auto seq = r.get(0, 0).and_then(parse_uint64);
    const auto same = r.get(0, 1).and_then(parse_bool);
    if (!seq || !same) {
        return std::unexpected(StoreError::Corrupt);
    }
    if (!*same) {
        return std::unexpected(StoreError::Conflict);
    }
    return seq;
}

// Keeps copies of the sender, key and body the statement binds: the caller's are views valid
// only during its call, and the pool sends the statement on a later iteration.
class AppendMessage final : public Operation {
public:
    AppendMessage(const core::RoomId& room, std::uint64_t generation, const rt::Outgoing& message,
                  StoreCallback<std::optional<std::uint64_t>> done)
        : room_(room), generation_(generation), sender_(message.sender), key_(message.key.view()),
          body_(message.body.begin(), message.body.end()), done_(std::move(done)) {}

    [[nodiscard]] Statement start() noexcept override {
        return Statement{.sql = kAppendMessage,
                         .params = Params{}
                                       .add_uuid(room_.uuid())
                                       .add_int(as_int(generation_))
                                       .add_text(sender_.view())
                                       .add_text(key_)
                                       .add_bytea(body_)};
    }

    [[nodiscard]] std::optional<Statement> next(Outcome outcome) noexcept override {
        // A concurrent repeat of this key won the insert; the next run finds its row. Counted
        // apart from start(), which the pool also calls to rerun, so that this happens once.
        if (!outcome && outcome.error() == DbError::Duplicate && !reran_) {
            reran_ = true;
            return start();
        }
        if (!outcome) {
            done_(std::unexpected(StoreError::Unavailable));
            return std::nullopt;
        }
        done_(decode_append(*outcome));
        return std::nullopt;
    }

    void abandon(DbError /*error*/) noexcept override {
        done_(std::unexpected(StoreError::Unavailable));
    }

private:
    core::RoomId room_;
    std::uint64_t generation_;
    core::UserId sender_;
    std::string key_;
    std::vector<std::byte> body_;
    bool reran_ = false;
    StoreCallback<std::optional<std::uint64_t>> done_;
};

class Advertise final : public Operation {
public:
    Advertise(const core::NodeId& node, std::string address, const core::Uuid& incarnation,
              StoreCallback<void> done)
        : node_(node), address_(std::move(address)), incarnation_(incarnation),
          done_(std::move(done)) {}

    [[nodiscard]] Statement start() noexcept override {
        return Statement{.sql = kAdvertise,
                         .params = Params{}
                                       .add_text(node_.view())
                                       .add_text(address_)
                                       .add_uuid(incarnation_)
                                       .add_int(stale_after_ms())};
    }

    [[nodiscard]] std::optional<Statement> next(Outcome outcome) noexcept override {
        if (!outcome) {
            done_(std::unexpected(StoreError::Unavailable));
        } else if (outcome->affected() == 0) {
            done_(std::unexpected(StoreError::NodeTaken));
        } else {
            done_({});
        }
        return std::nullopt;
    }

    void abandon(DbError /*error*/) noexcept override {
        done_(std::unexpected(StoreError::Unavailable));
    }

private:
    core::NodeId node_;
    std::string address_;
    core::Uuid incarnation_;
    StoreCallback<void> done_;
};

class ReadOwners final : public Operation {
public:
    using Owners = std::vector<std::pair<core::RoomId, Ownership>>;

    ReadOwners(const std::vector<core::RoomId>& rooms, StoreCallback<Owners> done)
        : rooms_(array_literal(rooms, [](const core::RoomId& room) { return room.to_string(); })),
          done_(std::move(done)) {}

    [[nodiscard]] Statement start() noexcept override {
        return Statement{.sql = kReadOwners, .params = Params{}.add_text(rooms_)};
    }

    [[nodiscard]] std::optional<Statement> next(Outcome outcome) noexcept override {
        if (!outcome) {
            done_(std::unexpected(StoreError::Unavailable));
            return std::nullopt;
        }
        done_(decode(*outcome));
        return std::nullopt;
    }

    void abandon(DbError /*error*/) noexcept override {
        done_(std::unexpected(StoreError::Unavailable));
    }

private:
    static StoreResult<Owners> decode(const Result& r) {
        Owners owners;
        owners.reserve(static_cast<std::size_t>(r.rows()));
        for (int row = 0; row < r.rows(); ++row) {
            const auto room = domain_at<core::RoomId>(r, row, 0);
            const auto node = domain_at<core::NodeId>(r, row, 1);
            const auto generation = generation_at(r, row, 2);
            if (!room || !node || !generation) {
                return std::unexpected(StoreError::Corrupt);
            }
            owners.emplace_back(*room, Ownership{.node = *node, .generation = *generation});
        }
        return owners;
    }

    std::string rooms_;
    StoreCallback<Owners> done_;
};

class FindAddress final : public Operation {
public:
    FindAddress(const core::NodeId& node, StoreCallback<std::optional<std::string>> done)
        : node_(node), done_(std::move(done)) {}

    [[nodiscard]] Statement start() noexcept override {
        return Statement{.sql = kFindAddress, .params = Params{}.add_text(node_.view())};
    }

    [[nodiscard]] std::optional<Statement> next(Outcome outcome) noexcept override {
        if (!outcome) {
            done_(std::unexpected(StoreError::Unavailable));
            return std::nullopt;
        }
        done_(outcome->get(0, 0).transform([](std::string_view a) { return std::string(a); }));
        return std::nullopt;
    }

    void abandon(DbError /*error*/) noexcept override {
        done_(std::unexpected(StoreError::Unavailable));
    }

private:
    core::NodeId node_;
    StoreCallback<std::optional<std::string>> done_;
};

// "<room> <generation> <node>", as notify_room_owner() writes it.
std::optional<std::pair<core::RoomId, Ownership>> parse_notice(std::string_view payload) {
    const std::size_t first = payload.find(' ');
    const std::size_t second =
        first == std::string_view::npos ? first : payload.find(' ', first + 1);
    if (second == std::string_view::npos) {
        return std::nullopt;
    }
    const auto room = core::RoomId::parse(payload.substr(0, first));
    const auto generation = parse_uint64(payload.substr(first + 1, second - first - 1));
    const auto node = core::NodeId::parse(payload.substr(second + 1));
    if (!room || !generation || *generation == 0 || !node) {
        return std::nullopt;
    }
    return std::pair{*room, Ownership{.node = *node, .generation = *generation}};
}

} // namespace

class PgRoomStore::Impl final : public INotificationSink {
public:
    Impl(net::IReactor& reactor, std::unique_ptr<Pool> pool) noexcept
        : pool_(std::move(pool)), refuse_(reactor, [this]() noexcept { answer_refused(); }) {}

    // Answered on the next iteration, never from inside the call that was refused.
    void refuse(StoreCallback<std::optional<std::uint64_t>> done) {
        refused_.push_back(std::move(done));
        refuse_.arm_unless_armed(core::Millis{0});
    }

    // The listening pool is made after this, since it points here.
    void listen_on(std::unique_ptr<Pool> listening) noexcept { listening_ = std::move(listening); }

    [[nodiscard]] Pool& pool() noexcept { return *pool_; }
    void watch(rt::IOwnershipListener& listener) noexcept { listener_ = &listener; }

    void on_listening() noexcept override {
        if (listener_ != nullptr) {
            listener_->on_resync();
        }
    }

    void on_notification(std::string_view payload) noexcept override {
        if (listener_ == nullptr) {
            return;
        }
        // Only the trigger writes to the channel; a payload it cannot have written means
        // something else did, and whatever it meant to say is unknown.
        const auto notice = parse_notice(payload);
        if (!notice) {
            listener_->on_resync();
            return;
        }
        listener_->on_owner_changed(notice->first, notice->second);
    }

private:
    void answer_refused() noexcept {
        std::vector<StoreCallback<std::optional<std::uint64_t>>> batch;
        batch.swap(refused_);
        // The router's store port has no "too large": to it this is a write that did not
        // happen, which it answers as unavailable (ADR-0052).
        for (auto& done : batch) {
            done(std::unexpected(StoreError::Unavailable));
        }
    }

    rt::IOwnershipListener* listener_ = nullptr;
    std::unique_ptr<Pool> pool_;
    std::vector<StoreCallback<std::optional<std::uint64_t>>> refused_;
    Timer refuse_;
    // Last: it calls into this object, and must stop before the members above go.
    std::unique_ptr<Pool> listening_;
};

std::expected<std::unique_ptr<PgRoomStore>, std::string>
PgRoomStore::create(net::IReactor& reactor, net::OffloadPool& offload,
                    const RoomStoreConfig& config) {
    auto pool = Pool::create(reactor, offload,
                             PoolConfig{.conninfo = config.conninfo,
                                        .application_name = "ulw-rooms",
                                        .connections = config.connections,
                                        .connect_timeout = config.connect_timeout,
                                        .request_timeout = rt::kStoreTimeout});
    if (!pool) {
        return std::unexpected(std::move(pool.error()));
    }
    auto impl = std::make_unique<Impl>(reactor, std::move(*pool));
    auto listening = Pool::create(reactor, offload,
                                  PoolConfig{.conninfo = config.conninfo,
                                             .application_name = "ulw-rooms-listen",
                                             .connections = 1,
                                             .connect_timeout = config.connect_timeout,
                                             .request_timeout = rt::kStoreTimeout,
                                             .listen = kListen,
                                             .notifications = impl.get()});
    if (!listening) {
        return std::unexpected(std::move(listening.error()));
    }
    impl->listen_on(std::move(*listening));
    return std::make_unique<PgRoomStore>(Token{}, std::move(impl));
}

PgRoomStore::PgRoomStore(Token /*token*/, std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

PgRoomStore::~PgRoomStore() = default;

void PgRoomStore::watch(rt::IOwnershipListener& listener) noexcept {
    impl_->watch(listener);
}

void PgRoomStore::resolve(const core::RoomId& room, const core::NodeId& node,
                          StoreCallback<Ownership> done) {
    impl_->pool().submit(std::make_unique<Resolve>(room, node, std::move(done)));
}

void PgRoomStore::claim_stale(std::vector<core::RoomId> rooms, const core::NodeId& node,
                              StoreCallback<std::vector<OwnedRoom>> done) {
    impl_->pool().submit(std::make_unique<ClaimStale>(rooms, node, std::move(done)));
}

void PgRoomStore::heartbeat(const core::NodeId& node, const core::Uuid& incarnation,
                            std::vector<OwnedRoom> rooms,
                            StoreCallback<std::vector<core::RoomId>> done) {
    impl_->pool().submit(std::make_unique<HeldRooms<std::vector<core::RoomId>>>(
        kHeartbeat, node, incarnation, rooms, &decode_renewed, std::move(done)));
}

// No message table exists yet: only the seq is taken. The message goes into the same statement
// once history is stored.
void PgRoomStore::append(const core::RoomId& room, std::uint64_t generation,
                         const rt::Outgoing& message,
                         StoreCallback<std::optional<std::uint64_t>> done) {
    // The client edge decodes no larger message (ADR-0029), and the node channel refuses a
    // larger body in any frame; a body here past the bound is refused, not trusted to fit.
    if (message.body.size() > core::ports::kMaxMessageBody) {
        impl_->refuse(std::move(done));
        return;
    }
    impl_->pool().submit(
        std::make_unique<AppendMessage>(room, generation, message, std::move(done)));
}

void PgRoomStore::release(const core::NodeId& node, std::vector<OwnedRoom> rooms,
                          StoreCallback<void> done) {
    impl_->pool().submit(std::make_unique<HeldRooms<void>>(kRelease, node, std::nullopt, rooms,
                                                           &decode_nothing, std::move(done)));
}

void PgRoomStore::advertise(const core::NodeId& node, std::string address,
                            const core::Uuid& incarnation, StoreCallback<void> done) {
    impl_->pool().submit(
        std::make_unique<Advertise>(node, std::move(address), incarnation, std::move(done)));
}

void PgRoomStore::read_owners(std::vector<core::RoomId> rooms,
                              StoreCallback<std::vector<std::pair<core::RoomId, Ownership>>> done) {
    impl_->pool().submit(std::make_unique<ReadOwners>(rooms, std::move(done)));
}

void PgRoomStore::find_address(const core::NodeId& node,
                               StoreCallback<std::optional<std::string>> done) {
    impl_->pool().submit(std::make_unique<FindAddress>(node, std::move(done)));
}

} // namespace infra::postgres
