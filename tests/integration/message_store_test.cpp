// What only the Postgres message store can show: its schema, its plans, its timings, and that
// what it wrote outlives the process that wrote it. The behaviour it shares with the in-memory
// store is in conformance/message_store_conformance_test.cpp.
#include "infra/postgres/message_store.hpp"
#include "infra/postgres/room_store.hpp"
#include "net/offload_pool.hpp"
#include "net/reactor_factory.hpp"
#include "os/system_clock.hpp"
#include "os/system_random.hpp"

#include "conformance/message_store_harness.hpp"
#include "message_sql.hpp"
#include "postgres_harness.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <format>
#include <gtest/gtest.h>
#include <limits>
#include <memory>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <vector>

namespace {

using core::ports::MessageResult;
using core::ports::StoredMessage;
using infra::postgres::Params;
using infra::postgres::PgMessageStore;
using infra::postgres::Sql;
using ulw::test::scalar;
using ulw::test::ScratchDatabase;
using Page = std::vector<StoredMessage>;

// "EXPLAIN (...) " followed by a history statement, built at compile time: Sql takes only
// constants.
constexpr std::string_view kExplain = "EXPLAIN (ANALYZE, BUFFERS, COSTS OFF) ";
template <std::size_t N> consteval auto explain_of(std::string_view statement) {
    std::array<char, N> out{};
    std::ranges::copy(kExplain, out.begin());
    std::ranges::copy(statement, out.begin() + kExplain.size());
    return out;
}
constexpr auto kExplainBefore =
    explain_of<kExplain.size() + infra::postgres::message_sql::kHistoryBeforeText.size() + 1>(
        infra::postgres::message_sql::kHistoryBeforeText);
constexpr auto kExplainAfter =
    explain_of<kExplain.size() + infra::postgres::message_sql::kHistoryAfterText.size() + 1>(
        infra::postgres::message_sql::kHistoryAfterText);

std::vector<std::byte> bytes(std::string_view text) {
    std::vector<std::byte> out(text.size());
    std::ranges::transform(text, out.begin(), [](char c) { return static_cast<std::byte>(c); });
    return out;
}

double millis(std::chrono::steady_clock::duration d) {
    return std::chrono::duration<double, std::milli>(d).count();
}

class MessageStoreTest : public ::testing::Test {
protected:
    void SetUp() override {
        ScratchDatabase::open(db_);
        if (IsSkipped() || HasFatalFailure()) {
            return;
        }
        conn_.emplace(db_->session());
        ASSERT_NO_FATAL_FAILURE(start());
    }

    void TearDown() override { stop(); }

    // A process's worth of store: its own loop, sessions and adapter.
    void start() {
        auto reactor = net::make_reactor(net::ReactorKind::Epoll, clock_, 1024);
        ASSERT_TRUE(reactor);
        reactor_ = std::move(*reactor);
        auto offload = net::OffloadPool::create(*reactor_, 1);
        ASSERT_TRUE(offload);
        offload_ = std::move(*offload);
        auto store = PgMessageStore::create(*reactor_, *offload_, {.conninfo = db_->conninfo()});
        ASSERT_TRUE(store) << store.error();
        store_ = std::move(*store);
        auto rooms = infra::postgres::PgRoomStore::create(*reactor_, *offload_,
                                                          {.conninfo = db_->conninfo()});
        ASSERT_TRUE(rooms) << rooms.error();
        rooms_ = std::move(*rooms);
    }

    void stop() {
        offload_.reset();
        rooms_.reset();
        store_.reset();
        reactor_.reset();
    }

    template <class T, class Call> MessageResult<T> ask(Call call) {
        return ulw::test::ask<T>(*reactor_, std::move(call));
    }

    // Takes the room as the owner would, and answers the generation to write under.
    std::uint64_t own(const core::RoomId& room) {
        const auto owner = ulw::test::ask_store<rt::Ownership>(
            *reactor_, [&](auto done) { rooms_->resolve(room, node_, std::move(done)); });
        EXPECT_TRUE(owner);
        return owner ? owner->generation : 0;
    }

    // The owner's write: the room's next seq and the message's row.
    rt::StoreResult<std::optional<std::uint64_t>>
    write(const core::RoomId& room, std::uint64_t generation, std::vector<std::byte> body) {
        return ulw::test::ask_store<std::optional<std::uint64_t>>(*reactor_, [&](auto done) {
            const std::string key = std::format("m{}", ++keys_);
            rooms_->append(room, generation, ulw::test::outgoing(alice_, key, body),
                           std::move(done));
        });
    }

    MessageResult<Page> before(const core::RoomId& room, std::optional<std::uint64_t> cursor,
                               std::size_t limit) {
        return ask<Page>(
            [&](auto done) { store_->history_before(room, cursor, limit, std::move(done)); });
    }

    // `count` rows for `room` straight into the table: seeding 10k messages through the
    // adapter would time the seeding, not the reads.
    void seed(const core::RoomId& room, std::int64_t count) {
        ASSERT_TRUE(conn_->exec(
            "INSERT INTO chat_messages (room_id, seq, sender, msg_key, body, sent_at) "
            "SELECT $1, n, 'auth0|alice', 'm' || n, convert_to('message ' || n, 'UTF8'), "
            "now() "
            "FROM generate_series(1, $2) AS n",
            Params{}.add_uuid(room.uuid()).add_int(count)));
    }

    std::string explain(Sql sql, const core::RoomId& room, std::int64_t cursor) {
        auto plan = conn_->exec(sql, Params{}
                                         .add_uuid(room.uuid())
                                         .add_int(cursor)
                                         .add_int(100)
                                         .add_int(core::ports::kMaxHistoryBytes));
        if (!plan) {
            ADD_FAILURE() << plan.error().message;
            return {};
        }
        std::string text;
        for (int row = 0; row < plan->rows(); ++row) {
            text += plan->get(row, 0).value_or("");
            text += '\n';
        }
        return text;
    }

    core::RoomId new_room() { return core::RoomId::generate(clock_, random_); }
    // A room with an id derived from a name (version 8) under the tag `tag`; 01, a stream's
    // chat, is the only kind record_live opens.
    core::RoomId named_room(std::string_view tag) {
        std::string text = new_room().to_string();
        text[14] = '8';
        text.replace(0, 2, tag);
        return *core::RoomId::parse(text);
    }
    core::RoomId stream_room() { return named_room("01"); }

    os::SystemClock clock_;
    os::SystemRandom random_;
    std::unique_ptr<ScratchDatabase> db_;
    std::optional<infra::postgres::SyncConnection> conn_;
    std::unique_ptr<net::IReactor> reactor_;
    std::unique_ptr<net::OffloadPool> offload_;
    std::unique_ptr<PgMessageStore> store_;
    std::unique_ptr<infra::postgres::PgRoomStore> rooms_;
    const core::NodeId node_ = *core::NodeId::parse("chat-a");
    std::uint64_t keys_ = 0;
    const core::UserId alice_ = *core::UserId::parse("auth0|alice");
};

// A member added by the store's own statement, whose transaction has not committed, when the
// server tries to open the room: the member's statement recorded the room closed first, so
// record_live waits on that record and is refused.
TEST_F(MessageStoreTest, AMemberInsertInFlightKeepsTheRoomClosedToRecordLive) {
    const core::RoomId room = stream_room();
    auto adding = db_->session();
    ASSERT_TRUE(adding.exec("BEGIN"));
    ASSERT_TRUE(adding.exec(infra::postgres::message_sql::kAddMember,
                            Params{}.add_uuid(room.uuid()).add_text(alice_.view())));

    std::optional<MessageResult<void>> recorded;
    store_->record_live(room, [&](MessageResult<void> r) noexcept { recorded = r; });
    ASSERT_TRUE(ulw::test::pump_until(*reactor_, [&] {
        return scalar(*conn_, "SELECT count(*) FROM pg_stat_activity WHERE "
                              "datname = current_database() AND wait_event_type = 'Lock'") == "1";
    }));
    ASSERT_TRUE(adding.exec("COMMIT"));
    ASSERT_TRUE(ulw::test::pump_until(*reactor_, [&] { return recorded.has_value(); }));

    EXPECT_EQ(*recorded,
              MessageResult<void>{std::unexpected(core::ports::MessageStoreError::Conflict)});
    EXPECT_EQ(scalar(*conn_, "SELECT kind FROM chat_rooms WHERE room_id = $1",
                     Params{}.add_uuid(room.uuid())),
              "group_chat");
}

// The same member insert in flight when the room's first joins arrive: one that asks for live
// is refused at once and records nothing; one that asks for a group chat waits on the closed
// record and takes it. Neither can leave the room open.
TEST_F(MessageStoreTest, AMemberInsertInFlightAndAFirstJoinNeverLeaveTheRoomOpen) {
    const core::RoomId room = new_room();
    const core::UserId bob = *core::UserId::parse("auth0|bob");
    auto adding = db_->session();
    ASSERT_TRUE(adding.exec("BEGIN"));
    ASSERT_TRUE(adding.exec(infra::postgres::message_sql::kAddMember,
                            Params{}.add_uuid(room.uuid()).add_text(alice_.view())));

    EXPECT_EQ(ask<core::ports::Admission>([&](auto done) {
                  store_->admits(room, bob, core::ports::RoomKind::StreamLiveChat, std::move(done));
              }),
              core::ports::Admission::NotLive);

    std::optional<MessageResult<core::ports::Admission>> grouped;
    store_->admits(room, bob, core::ports::RoomKind::GroupChat,
                   [&](MessageResult<core::ports::Admission> r) noexcept { grouped = r; });
    ASSERT_TRUE(ulw::test::pump_until(*reactor_, [&] {
        return scalar(*conn_, "SELECT count(*) FROM pg_stat_activity WHERE "
                              "datname = current_database() AND wait_event_type = 'Lock'") == "1";
    }));
    ASSERT_TRUE(adding.exec("COMMIT"));
    ASSERT_TRUE(ulw::test::pump_until(*reactor_, [&] { return grouped.has_value(); }));

    EXPECT_EQ(*grouped, core::ports::Admission::NotMember);
    EXPECT_EQ(ask<core::ports::Admission>([&](auto done) {
                  store_->admits(room, alice_, core::ports::RoomKind::StreamLiveChat,
                                 std::move(done));
              }),
              core::ports::Admission::NotLive);
    EXPECT_EQ(scalar(*conn_, "SELECT kind FROM chat_rooms WHERE room_id = $1",
                     Params{}.add_uuid(room.uuid())),
              "group_chat");
}

// A room the room plane created closed keeps its kind and delivery in room_state, so it cannot be
// opened afterwards: chat_rooms and room_state would disagree. Its creation recorded it closed in
// chat_rooms as well, as a first join would have. One opened before it was created is created
// live, and opening it again still answers ok.
TEST_F(MessageStoreTest, RecordLiveRefusesARoomTheRoomPlaneCreatedClosed) {
    const core::RoomId closed = stream_room();
    ASSERT_NE(own(closed), 0U);
    EXPECT_EQ(ask<void>([&](auto done) { store_->record_live(closed, std::move(done)); }),
              MessageResult<void>{std::unexpected(core::ports::MessageStoreError::Conflict)});
    EXPECT_EQ(scalar(*conn_, "SELECT kind FROM chat_rooms WHERE room_id = $1",
                     Params{}.add_uuid(closed.uuid())),
              "group_chat");
    EXPECT_EQ(scalar(*conn_,
                     "SELECT concat_ws(' ', kind, delivery) FROM room_state "
                     "WHERE room_id = $1",
                     Params{}.add_uuid(closed.uuid())),
              "group_chat durable");

    const core::RoomId live = stream_room();
    ASSERT_TRUE(ask<void>([&](auto done) { store_->record_live(live, std::move(done)); }));
    ASSERT_NE(own(live), 0U);
    EXPECT_TRUE(ask<void>([&](auto done) { store_->record_live(live, std::move(done)); }));
    EXPECT_EQ(scalar(*conn_,
                     "SELECT concat_ws(' ', kind, delivery) FROM room_state "
                     "WHERE room_id = $1",
                     Params{}.add_uuid(live.uuid())),
              "stream_live_chat lossy");
}

// ADR-0070: the database itself refuses to record any other room live, so an operator's
// statement cannot open a room whose id every node takes for a closed one.
TEST_F(MessageStoreTest, OnlyAStreamsRoomCanBeRecordedLive) {
    // A version 7 room, and a presence room (version 8, tag 02).
    for (const core::RoomId& other : {new_room(), named_room("02")}) {
        EXPECT_FALSE(
            conn_->exec("INSERT INTO chat_rooms (room_id, kind) VALUES ($1, 'stream_live_chat')",
                        Params{}.add_uuid(other.uuid())));
        EXPECT_EQ(ask<void>([&](auto done) { store_->record_live(other, std::move(done)); }),
                  MessageResult<void>{std::unexpected(core::ports::MessageStoreError::Conflict)});
    }
    EXPECT_TRUE(
        conn_->exec("INSERT INTO chat_rooms (room_id, kind) VALUES ($1, 'stream_live_chat')",
                    Params{}.add_uuid(stream_room().uuid())));
    EXPECT_TRUE(conn_->exec("INSERT INTO chat_rooms (room_id, kind) VALUES ($1, 'group_chat')",
                            Params{}.add_uuid(stream_room().uuid())));
}

// The server opening a room, its transaction not yet committed, when the room plane creates the
// room with no join before it (as it creates presence rooms): the creation records the room in
// chat_rooms in its own statement, so it waits on the open record and takes the live kind. Were
// it to read chat_rooms instead, it would miss the record, create the room closed, and leave
// chat_rooms saying live once the server's transaction committed.
TEST_F(MessageStoreTest, ARoomCreatedWhileTheServerOpensItIsCreatedLive) {
    const core::RoomId room = stream_room();
    auto opening = db_->session();
    ASSERT_TRUE(opening.exec("BEGIN"));
    ASSERT_EQ(
        scalar(opening, infra::postgres::message_sql::kRecordLive, Params{}.add_uuid(room.uuid())),
        "stream_live_chat");

    std::optional<rt::StoreResult<rt::Ownership>> created;
    rooms_->resolve(room, node_, [&](rt::StoreResult<rt::Ownership> r) noexcept { created = r; });
    ASSERT_TRUE(ulw::test::pump_until(*reactor_, [&] {
        return created.has_value() ||
               scalar(*conn_, "SELECT count(*) FROM pg_stat_activity WHERE "
                              "datname = current_database() AND wait_event_type = 'Lock'") == "1";
    }));
    EXPECT_FALSE(created.has_value()) << "the creation did not wait on the open record";
    ASSERT_TRUE(opening.exec("COMMIT"));
    ASSERT_TRUE(ulw::test::pump_until(*reactor_, [&] { return created.has_value(); }));

    ASSERT_TRUE(*created);
    EXPECT_EQ((*created)->generation, 1U);
    EXPECT_EQ(scalar(*conn_,
                     "SELECT concat_ws(' ', r.kind, s.kind, s.delivery) "
                     "FROM chat_rooms r JOIN room_state s USING (room_id) WHERE room_id = $1",
                     Params{}.add_uuid(room.uuid())),
              "stream_live_chat stream_live_chat lossy");
}

// A presence room is created with no join, and recorded closed as it is created: the server
// cannot open it afterwards, and room_state keeps it a presence room.
TEST_F(MessageStoreTest, APresenceRoomIsRecordedClosedAsItIsCreated) {
    std::string text = new_room().to_string();
    text.replace(0, 2, "02");
    text[14] = '8';
    const core::RoomId room = *core::RoomId::parse(text);
    ASSERT_TRUE(rt::is_ephemeral_room(room));
    ASSERT_NE(own(room), 0U);
    EXPECT_EQ(ask<void>([&](auto done) { store_->record_live(room, std::move(done)); }),
              MessageResult<void>{std::unexpected(core::ports::MessageStoreError::Conflict)});
    EXPECT_EQ(scalar(*conn_,
                     "SELECT concat_ws(' ', r.kind, s.kind, s.delivery) "
                     "FROM chat_rooms r JOIN room_state s USING (room_id) WHERE room_id = $1",
                     Params{}.add_uuid(room.uuid())),
              "group_chat presence durable");
}

// A join reads a recorded room's kind and writes nothing.
TEST_F(MessageStoreTest, AJoinOfARecordedRoomWritesNothing) {
    const core::RoomId room = new_room();
    ASSERT_TRUE(ask<void>([&](auto done) { store_->add_member(room, alice_, std::move(done)); }));
    const auto xmin = [&] {
        return scalar(*conn_, "SELECT xmin::text FROM chat_rooms WHERE room_id = $1",
                      Params{}.add_uuid(room.uuid()));
    };
    const std::string before = xmin();
    EXPECT_EQ(ask<core::ports::Admission>([&](auto done) {
                  store_->admits(room, alice_, core::ports::RoomKind::GroupChat, std::move(done));
              }),
              core::ports::Admission::Admitted);
    EXPECT_EQ(xmin(), before);
}

// Membership is what keeps a closed room closed: a member taken off is refused at the next join,
// and taking them off again, or taking off someone never added, changes nothing.
TEST_F(MessageStoreTest, AMemberTakenOffAClosedRoomIsNoLongerAdmitted) {
    const core::RoomId room = new_room();
    const core::UserId bob = *core::UserId::parse("auth0|bob");
    const auto admits = [&](const core::UserId& user) {
        return ask<core::ports::Admission>([&](auto done) {
            store_->admits(room, user, core::ports::RoomKind::GroupChat, std::move(done));
        });
    };
    ASSERT_TRUE(ask<void>([&](auto done) { store_->add_member(room, alice_, std::move(done)); }));
    ASSERT_TRUE(ask<void>([&](auto done) { store_->add_member(room, bob, std::move(done)); }));
    EXPECT_EQ(admits(bob), core::ports::Admission::Admitted);

    ASSERT_TRUE(ask<void>([&](auto done) { store_->remove_member(room, bob, std::move(done)); }));
    EXPECT_EQ(admits(bob), core::ports::Admission::NotMember);
    EXPECT_EQ(admits(alice_), core::ports::Admission::Admitted);
    EXPECT_TRUE(ask<void>([&](auto done) { store_->remove_member(room, bob, std::move(done)); }));
    const core::UserId never = *core::UserId::parse("auth0|never");
    EXPECT_TRUE(ask<void>([&](auto done) { store_->remove_member(room, never, std::move(done)); }));

    // The last member gone leaves the room closed, not open to anyone.
    ASSERT_TRUE(
        ask<void>([&](auto done) { store_->remove_member(room, alice_, std::move(done)); }));
    EXPECT_EQ(admits(alice_), core::ports::Admission::NotMember);
    EXPECT_EQ(ask<core::ports::Admission>([&](auto done) {
                  store_->admits(room, bob, core::ports::RoomKind::StreamLiveChat, std::move(done));
              }),
              core::ports::Admission::NotLive);
}

// Pages of members in byte order of their ids, whatever the database's own collation, each
// starting after the last id of the one before.
TEST_F(MessageStoreTest, MembersArePagedInByteOrderAfterTheLastIdSeen) {
    const core::RoomId room = new_room();
    const core::RoomId other = new_room();
    // Upper case sorts before lower case in bytes, and after it in en_US.
    const std::vector<std::string> ids{"auth0|Zed", "auth0|alice", "auth0|bob", "auth0|carol",
                                       "google|dave"};
    for (const auto& id : {ids[3], ids[1], ids[4], ids[0], ids[2]}) {
        const core::UserId user = *core::UserId::parse(id);
        ASSERT_TRUE(ask<void>([&](auto done) { store_->add_member(room, user, std::move(done)); }));
    }
    const core::UserId stranger = *core::UserId::parse("auth0|aaron");
    ASSERT_TRUE(
        ask<void>([&](auto done) { store_->add_member(other, stranger, std::move(done)); }));
    // Adding again is idempotent.
    ASSERT_TRUE(ask<void>([&](auto done) { store_->add_member(room, alice_, std::move(done)); }));

    using Members = std::vector<core::UserId>;
    const auto page = [&](std::optional<core::UserId> after, std::size_t limit) {
        return ask<Members>(
            [&](auto done) { store_->members(room, std::move(after), limit, std::move(done)); });
    };
    const auto names = [](const Members& members) {
        std::vector<std::string> out;
        for (const auto& m : members) {
            out.emplace_back(m.view());
        }
        return out;
    };
    const auto first = page(std::nullopt, 2);
    ASSERT_TRUE(first);
    EXPECT_EQ(names(*first), (std::vector<std::string>{ids[0], ids[1]}));
    const auto second = page(first->back(), 2);
    ASSERT_TRUE(second);
    EXPECT_EQ(names(*second), (std::vector<std::string>{ids[2], ids[3]}));
    const auto last = page(second->back(), 2);
    ASSERT_TRUE(last);
    EXPECT_EQ(names(*last), (std::vector<std::string>{ids[4]}));
    const auto past = page(last->back(), 2);
    ASSERT_TRUE(past);
    EXPECT_TRUE(past->empty());
    const auto all = page(std::nullopt, 100);
    ASSERT_TRUE(all);
    EXPECT_EQ(names(*all), ids);
}

TEST_F(MessageStoreTest, BodiesAreByteaNeverTextAndNothingIndexesThem) {
    EXPECT_EQ(scalar(*conn_,
                     "SELECT data_type || ' ' || is_nullable FROM information_schema.columns "
                     "WHERE table_name = 'chat_messages' AND column_name = 'body'"),
              "bytea NO");
    EXPECT_EQ(scalar(*conn_, "SELECT format_type(atttypid, atttypmod) FROM pg_attribute "
                             "WHERE attrelid = 'chat_messages'::regclass AND attname = 'body'"),
              "bytea");
    EXPECT_EQ(scalar(*conn_, "SELECT count(*) FROM pg_index i JOIN pg_attribute a "
                             "ON a.attrelid = i.indrelid AND a.attnum = ANY (i.indkey) "
                             "WHERE i.indrelid = 'chat_messages'::regclass AND a.attname = 'body'"),
              "0");
    EXPECT_EQ(scalar(*conn_,
                     "SELECT string_agg(a.attname, ',' ORDER BY k.i) FROM pg_index x "
                     "CROSS JOIN LATERAL unnest(x.indkey) WITH ORDINALITY AS k (attnum, i) "
                     "JOIN pg_attribute a ON a.attrelid = x.indrelid AND a.attnum = k.attnum "
                     "WHERE x.indrelid = 'chat_messages'::regclass AND x.indisprimary"),
              "room_id,seq");
}

TEST_F(MessageStoreTest, HistorySurvivesARestartInTheSameOrder) {
    const core::RoomId room = new_room();
    const std::uint64_t generation = own(room);
    for (std::uint64_t seq = 1; seq <= 5; ++seq) {
        ASSERT_EQ(write(room, generation, bytes(std::format("body {}", seq))),
                  (rt::StoreResult<std::optional<std::uint64_t>>{seq}));
    }
    const auto first = before(room, std::nullopt, 10);
    ASSERT_TRUE(first);

    stop();
    ASSERT_NO_FATAL_FAILURE(start());

    const auto again = before(room, std::nullopt, 10);
    ASSERT_TRUE(again);
    EXPECT_EQ(*again, *first);
    std::vector<std::uint64_t> order;
    for (const StoredMessage& m : *again) {
        order.push_back(m.seq);
        EXPECT_EQ(m.body, bytes(std::format("body {}", m.seq)));
    }
    EXPECT_EQ(order, (std::vector<std::uint64_t>{5, 4, 3, 2, 1}));
    EXPECT_EQ(ask<std::uint64_t>([&](auto done) { store_->last_seq(room, std::move(done)); }), 5U);
}

TEST_F(MessageStoreTest, ABodyOverThePagesByteBoundStillMakesAPageOfItsOwn) {
    const core::RoomId room = new_room();
    // No writer here stores such a body; a row put there by hand must not end paging early.
    ASSERT_TRUE(conn_->exec(
        "INSERT INTO chat_messages (room_id, seq, sender, msg_key, body, sent_at) VALUES "
        "($1, 1, 'auth0|alice', 'k1', '\\x01', now()), "
        "($1, 2, 'auth0|alice', 'k2', decode(repeat('ab', $2::integer), 'hex'), now()), "
        "($1, 3, 'auth0|alice', 'k3', '\\x03', now())",
        Params{}.add_uuid(room.uuid()).add_int(core::ports::kMaxHistoryBytes + 1)));
    const auto seqs_of = [](const MessageResult<Page>& page) {
        std::vector<std::uint64_t> out;
        for (const StoredMessage& m : page.value_or(Page{})) {
            out.push_back(m.seq);
        }
        return out;
    };
    const auto after = [&](std::uint64_t cursor) {
        return ask<Page>(
            [&](auto done) { store_->history_after(room, cursor, 10, std::move(done)); });
    };
    EXPECT_EQ(seqs_of(after(0)), std::vector<std::uint64_t>{1});
    const auto large = after(1);
    EXPECT_EQ(seqs_of(large), std::vector<std::uint64_t>{2});
    EXPECT_EQ(large->front().body.size(), core::ports::kMaxHistoryBytes + 1);
    EXPECT_EQ(seqs_of(after(2)), std::vector<std::uint64_t>{3});
    EXPECT_EQ(seqs_of(before(room, std::nullopt, 10)), std::vector<std::uint64_t>{3});
    EXPECT_EQ(seqs_of(before(room, 3, 10)), std::vector<std::uint64_t>{2});
    EXPECT_EQ(seqs_of(before(room, 2, 10)), std::vector<std::uint64_t>{1});
}

TEST_F(MessageStoreTest, HistoryOfATenThousandMessageRoomWalksThePrimaryKey) {
    const core::RoomId room = new_room();
    ASSERT_NO_FATAL_FAILURE(seed(room, 10'000));
    // Other rooms around it, so that a plan that is not keyed on the room shows.
    for (int i = 0; i < 4; ++i) {
        ASSERT_NO_FATAL_FAILURE(seed(new_room(), 10'000));
    }
    ASSERT_TRUE(conn_->exec("ANALYZE chat_messages"));

    struct Probe {
        std::string_view name;
        Sql sql;
        std::int64_t cursor;
        std::string_view scan;
    };
    const std::array probes{
        Probe{.name = "newest page",
              .sql = kExplainBefore.data(),
              .cursor = std::numeric_limits<std::int64_t>::max(),
              .scan = "Index Scan Backward using chat_messages_pkey"},
        Probe{.name = "page below seq 5000",
              .sql = kExplainBefore.data(),
              .cursor = 5000,
              .scan = "Index Scan Backward using chat_messages_pkey"},
        Probe{.name = "page above seq 5000",
              .sql = kExplainAfter.data(),
              .cursor = 5000,
              .scan = "Index Scan using chat_messages_pkey"},
    };
    for (const Probe& p : probes) {
        const std::string plan = explain(p.sql, room, p.cursor);
        std::println("EXPLAIN ANALYZE, {}:\n{}", p.name, plan);
        EXPECT_NE(plan.find(p.scan), std::string::npos) << plan;
        EXPECT_EQ(plan.find("Seq Scan"), std::string::npos) << plan;
        EXPECT_EQ(plan.find("Sort"), std::string::npos) << plan;
        // The scan stops at the page: 100 rows plus the one the window reads ahead.
        EXPECT_NE(plan.find("rows=101 "), std::string::npos) << plan;
    }

    // The whole room, page by page, through the adapter: what a client scrolling to the top
    // would cost, in round trips.
    const auto started = std::chrono::steady_clock::now();
    std::optional<std::uint64_t> cursor;
    std::size_t pages = 0;
    std::size_t messages = 0;
    for (;;) {
        const auto page = before(room, cursor, 100);
        ASSERT_TRUE(page);
        if (page->empty()) {
            break;
        }
        ++pages;
        messages += page->size();
        cursor = page->back().seq;
    }
    const double took = millis(std::chrono::steady_clock::now() - started);
    EXPECT_EQ(messages, 10'000U);
    std::println("paged 10000 messages back in {} pages of 100: {:.1f} ms, {:.2f} ms a page", pages,
                 took, took / static_cast<double>(pages));
    RecordProperty("page_10k_ms", std::format("{:.1f}", took));
}

// Every form a body could take in a log line: as sent, as bytea prints it, and as base64.
std::vector<std::string> forms_of(std::string_view text) {
    std::string hex;
    for (const char c : text) {
        hex += std::format("{:02x}", static_cast<unsigned char>(c));
    }
    constexpr std::string_view kAlphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string base64;
    std::uint32_t bits = 0;
    int held = 0;
    for (const char c : text) {
        bits = (bits << 8U) | static_cast<unsigned char>(c);
        held += 8;
        while (held >= 6) {
            held -= 6;
            base64 += kAlphabet[(bits >> static_cast<unsigned>(held)) & 0x3FU];
        }
    }
    // A trailing partial group depends on what follows the text; the whole groups before it
    // are what any encoding of a body holding the text contains.
    return {std::string{text}, hex, base64};
}

bool holds_any(std::string_view haystack, const std::vector<std::string>& forms) {
    return std::ranges::any_of(
        forms, [&](const std::string& f) { return haystack.find(f) != std::string_view::npos; });
}

class PlaintextTest : public MessageStoreTest {
protected:
    // One owner's write of `marker` under the given session settings, and one that fails on a
    // row already where its seq would go, so that the server reports an error carrying it.
    void write_twice(const std::string& options, const std::string& marker) {
        std::string conninfo = db_->conninfo();
        if (!options.empty()) {
            conninfo += " options='" + options + "'";
        }
        auto created =
            infra::postgres::PgRoomStore::create(*reactor_, *offload_, {.conninfo = conninfo});
        ASSERT_TRUE(created) << created.error();
        auto& rooms = sessions_.emplace_back(std::move(*created));
        const core::RoomId room = new_room();
        const auto owner = ulw::test::ask_store<rt::Ownership>(
            *reactor_, [&](auto done) { rooms->resolve(room, node_, std::move(done)); });
        ASSERT_TRUE(owner);
        const auto write = [&](std::string_view key) {
            return ulw::test::ask_store<std::optional<std::uint64_t>>(*reactor_, [&](auto done) {
                const std::vector<std::byte> body = bytes(marker);
                rooms->append(room, owner->generation, ulw::test::outgoing(alice_, key, body),
                              std::move(done));
            });
        };
        ASSERT_EQ(write("k1"), (rt::StoreResult<std::optional<std::uint64_t>>{1}));
        ASSERT_TRUE(conn_->exec("INSERT INTO chat_messages (room_id, seq, sender, msg_key, body, "
                                "sent_at) VALUES ($1, 2, 'auth0|mallory', 'k', '\\x00', now())",
                                Params{}.add_uuid(room.uuid())));
        EXPECT_FALSE(write("k2"));
        EXPECT_TRUE(before(room, std::nullopt, 10));
    }

    // The server's log from `since` on, once it holds `last`: the log is one stream, so
    // everything written before `last` is in it by then.
    std::optional<std::string> log_through(const std::string& since, const std::string& last) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{15};
        while (std::chrono::steady_clock::now() < deadline) {
            auto log = ulw::test::run_process({"docker", "logs", "--since", since, *container_});
            if (log.exit_code != 0) {
                ADD_FAILURE() << log.output;
                return std::nullopt;
            }
            if (log.output.find(last) != std::string::npos) {
                return std::move(log.output);
            }
        }
        ADD_FAILURE() << "the server's log never showed the logged body";
        return std::nullopt;
    }

    std::string new_marker() { return "plaintext-" + core::Uuid::v7(clock_, random_).to_string(); }

    void TearDown() override {
        // The offload pool stops before the stores it may be resolving for.
        offload_.reset();
        sessions_.clear();
        MessageStoreTest::TearDown();
    }

    std::optional<std::string> container_ = ulw::test::postgres_container();
    std::vector<std::unique_ptr<infra::postgres::PgRoomStore>> sessions_;
};

TEST_F(PlaintextTest, NoBodyReachesAProcessLogTheServerLogOrATextColumn) {
    // What ADR-0054 asks of production Postgres, applied to these sessions: every statement
    // logged, and still no parameter in the log, on success or on error.
    constexpr std::string_view kProduction = "-c log_statement=all -c log_parameter_max_length=0 "
                                             "-c log_parameter_max_length_on_error=0";
    // What it forbids: the same, with parameters logged in full.
    constexpr std::string_view kLeaky = "-c log_statement=all -c log_parameter_max_length=-1 "
                                        "-c log_parameter_max_length_on_error=-1";
    EXPECT_EQ(scalar(*conn_, "SHOW log_parameter_max_length_on_error"), "0");

    const std::string as_configured = new_marker();
    const std::string production = new_marker();
    const std::string leaked = new_marker();
    const auto since = std::format(
        "{:%FT%TZ}", std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()));

    ::testing::internal::CaptureStderr();
    ::testing::internal::CaptureStdout();
    write_twice("", as_configured);
    write_twice(std::string{kProduction}, production);
    write_twice(std::string{kLeaky}, leaked);
    const std::string out = ::testing::internal::GetCapturedStdout();
    const std::string err = ::testing::internal::GetCapturedStderr();
    for (const std::string& marker : {as_configured, production, leaked}) {
        EXPECT_FALSE(holds_any(out, forms_of(marker)));
        EXPECT_FALSE(holds_any(err, forms_of(marker)));
    }

    // Read back to this process and searched here, so that no statement carries a marker.
    std::string activity;
    const auto queries = conn_->exec("SELECT coalesce(string_agg(query, ' '), '') FROM "
                                     "pg_stat_activity WHERE pid <> pg_backend_pid()");
    ASSERT_TRUE(queries);
    activity = queries->get(0, 0).value_or("");
    const auto text =
        conn_->exec("SELECT coalesce(string_agg(sender || ' ' || msg_key, ' '), '') FROM "
                    "chat_messages");
    ASSERT_TRUE(text);
    const std::string columns{text->get(0, 0).value_or("")};
    for (const std::string& marker : {as_configured, production, leaked}) {
        EXPECT_FALSE(holds_any(activity, forms_of(marker)));
        EXPECT_FALSE(holds_any(columns, forms_of(marker)));
    }

    if (!container_) {
        GTEST_SKIP() << "no docker access to the server's log; the process checks above ran";
    }
    // The leaked body shows that the search finds a logged body in the form the server writes.
    const auto log = log_through(since, forms_of(leaked)[1]);
    ASSERT_TRUE(log);
    EXPECT_FALSE(holds_any(*log, forms_of(as_configured)));
    EXPECT_FALSE(holds_any(*log, forms_of(production)));
}

} // namespace
