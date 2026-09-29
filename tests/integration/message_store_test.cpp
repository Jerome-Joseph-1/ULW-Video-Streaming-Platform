// What only the Postgres message store can show: its schema, its plans, its timings, and that
// what it wrote outlives the process that wrote it. The behaviour it shares with the in-memory
// store is in conformance/message_store_conformance_test.cpp.
#include "infra/postgres/message_store.hpp"
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

core::WallTime at(std::int64_t second) {
    return core::WallTime{std::chrono::seconds{1'790'000'000 + second}};
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
    }

    void stop() {
        offload_.reset();
        store_.reset();
        reactor_.reset();
    }

    template <class T, class Call> MessageResult<T> ask(Call call) {
        return ulw::test::ask<T>(*reactor_, std::move(call));
    }

    MessageResult<void> append(const core::RoomId& room, std::uint64_t seq,
                               std::vector<std::byte> body) {
        return ask<void>([&](auto done) {
            store_->append(room, seq, alice_, std::move(body), at(0), std::move(done));
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
        ASSERT_TRUE(
            conn_->exec("INSERT INTO chat_messages (room_id, seq, sender, body, sent_at) "
                        "SELECT $1, n, 'auth0|alice', convert_to('message ' || n, 'UTF8'), now() "
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
    // Appended out of order, as retries after a lost answer can land them.
    for (const std::uint64_t seq : std::array<std::uint64_t, 5>{3, 1, 2, 5, 4}) {
        ASSERT_TRUE(append(room, seq, bytes(std::format("body {}", seq))));
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

TEST_F(MessageStoreTest, AnAppendIsOneRoundTripAndACommit) {
    // An owner appends a room's messages one at a time (ADR-0035), each before its delivery,
    // so this is what every message of a durable room waits for. A read by primary key beside
    // it separates the round trip from the commit's WAL flush.
    constexpr std::size_t kCount = 500;
    const core::RoomId room = new_room();
    std::vector<double> appends;
    std::vector<double> reads;
    for (std::uint64_t seq = 1; seq <= kCount; ++seq) {
        auto started = std::chrono::steady_clock::now();
        ASSERT_TRUE(append(room, seq, bytes("a line of chat, about forty bytes long")));
        appends.push_back(millis(std::chrono::steady_clock::now() - started));
        started = std::chrono::steady_clock::now();
        ASSERT_TRUE(
            ask<std::uint64_t>([&](auto done) { store_->last_seq(room, std::move(done)); }));
        reads.push_back(millis(std::chrono::steady_clock::now() - started));
    }
    const auto quantile = [](std::vector<double>& samples, double q) {
        std::ranges::sort(samples);
        return samples[static_cast<std::size_t>(q * static_cast<double>(samples.size() - 1))];
    };
    const double append_p50 = quantile(appends, 0.5);
    const double append_p99 = quantile(appends, 0.99);
    const double read_p50 = quantile(reads, 0.5);
    std::println(
        "{} sequential appends: p50 {:.3f} ms, p99 {:.3f} ms; a read by key: p50 {:.3f} ms", kCount,
        append_p50, append_p99, read_p50);
    RecordProperty("append_p50_ms", std::format("{:.3f}", append_p50));
    RecordProperty("append_p99_ms", std::format("{:.3f}", append_p99));
    RecordProperty("read_p50_ms", std::format("{:.3f}", read_p50));
    // Loose: the check is that an append is one statement, not a batch window or a retry loop.
    EXPECT_LT(append_p50, 50.0);
}

TEST_F(MessageStoreTest, APlaintextBodyNeverReachesALogOrAnError) {
    const std::string marker = "plaintext-" + core::Uuid::v7(clock_, random_).to_string();
    const core::RoomId room = new_room();
    const auto since = std::format(
        "{:%FT%TZ}", std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()));

    ::testing::internal::CaptureStderr();
    ::testing::internal::CaptureStdout();
    ASSERT_TRUE(append(room, 1, bytes(marker)));
    // Every path a body takes: a repeat, a conflict, an oversized body, and reads.
    EXPECT_TRUE(append(room, 1, bytes(marker)));
    EXPECT_FALSE(append(room, 1, bytes(marker + " edited")));
    std::string huge = marker;
    huge.resize(core::ports::kMaxMessageBody + 1, 'x');
    EXPECT_FALSE(append(room, 2, bytes(huge)));
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
