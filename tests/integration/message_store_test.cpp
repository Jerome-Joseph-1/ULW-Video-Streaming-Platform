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
            rooms_->append_message(room, generation, alice_, std::format("m{}", ++keys_), body,
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

TEST_F(MessageStoreTest, APlaintextBodyNeverReachesALogOrAnError) {
    const std::string marker = "plaintext-" + core::Uuid::v7(clock_, random_).to_string();
    const core::RoomId room = new_room();
    const auto since = std::format(
        "{:%FT%TZ}", std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()));

    ::testing::internal::CaptureStderr();
    ::testing::internal::CaptureStdout();
    const std::uint64_t generation = own(room);
    ASSERT_TRUE(write(room, generation, bytes(marker)));
    EXPECT_TRUE(before(room, std::nullopt, 10));
    const std::string out = ::testing::internal::GetCapturedStdout();
    const std::string err = ::testing::internal::GetCapturedStderr();
    EXPECT_EQ(out.find(marker), std::string::npos);
    EXPECT_EQ(err.find(marker), std::string::npos);

    // Statements carry bodies as bound parameters: their text, which the server shows in
    // pg_stat_activity and writes with any error, never holds one.
    EXPECT_EQ(scalar(*conn_,
                     "SELECT count(*) FROM pg_stat_activity WHERE strpos(query, $1) > 0 "
                     "AND pid <> pg_backend_pid()",
                     Params{}.add_text(marker)),
              "0");
    // Nor does any text column.
    EXPECT_EQ(scalar(*conn_, "SELECT count(*) FROM chat_messages WHERE strpos(sender, $1) > 0",
                     Params{}.add_text(marker)),
              "0");

    const auto container = ulw::test::postgres_container();
    if (!container) {
        GTEST_SKIP() << "no docker access to the server's log; the process checks above ran";
    }
    const auto log = ulw::test::run_process({"docker", "logs", "--since", since, *container});
    ASSERT_EQ(log.exit_code, 0) << log.output;
    EXPECT_EQ(log.output.find(marker), std::string::npos);
}

} // namespace
