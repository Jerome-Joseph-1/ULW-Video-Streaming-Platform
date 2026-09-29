#include "net/offload_pool.hpp"
#include "net/reactor_factory.hpp"
#include "net/socket.hpp"
#include "os/system_clock.hpp"

#include "operation.hpp"
#include "pool.hpp"
#include "postgres_harness.hpp"
#include "support/reactor_harness.hpp"

#include <sys/socket.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <expected>
#include <format>
#include <gtest/gtest.h>
#include <latch>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using infra::postgres::DbError;
using infra::postgres::Operation;
using infra::postgres::Outcome;
using infra::postgres::Params;
using infra::postgres::Pool;
using infra::postgres::PoolConfig;
using infra::postgres::Query;
using infra::postgres::Sql;
using infra::postgres::Statement;
using ulw::test::scalar;
using ulw::test::ScratchDatabase;
using Clock = std::chrono::steady_clock;
using Answer = std::expected<std::string, DbError>;

// Well above any slice the loop takes when nothing blocks it (a few ms, more under ASan), and
// well below the 1 s timeouts these tests use: a blocking libpq call would show as 1 s or more.
constexpr auto kLongestAcceptableSlice = std::chrono::milliseconds(250);
constexpr core::Millis kTimeout{1000};
constexpr core::Millis kTick{100};
constexpr auto kObserved = std::chrono::milliseconds(2500);
// The wheel rounds a deadline up to the tick after next, so a timer re-armed every 100 ms
// fires every 200 ms: 12 times in 2.5 s. One stall as long as kTimeout would cost 5 of them.
constexpr int kMinimumTicks = 10;

// Re-arms itself every tick; it keeps counting only while nothing blocks the loop.
class Ticker final : public net::ITimerHandler {
public:
    explicit Ticker(net::IReactor& reactor) : reactor_(reactor) {}
    ~Ticker() override {
        if (started_) {
            reactor_.cancel_timer(timer_);
        }
    }
    Ticker(const Ticker&) = delete;
    Ticker& operator=(const Ticker&) = delete;
    Ticker(Ticker&&) = delete;
    Ticker& operator=(Ticker&&) = delete;

    void start() {
        timer_ = reactor_.arm_timer(kTick, *this);
        started_ = true;
    }
    void on_timeout() noexcept override {
        ++ticks_;
        timer_ = reactor_.arm_timer(kTick, *this);
    }
    [[nodiscard]] int ticks() const noexcept { return ticks_; }

private:
    net::IReactor& reactor_;
    net::TimerId timer_;
    bool started_ = false;
    int ticks_ = 0;
};

// Occupies an offload thread until released, holding back every job queued behind it.
class OffloadBlocker final : public net::IOffloadJob {
public:
    void run() noexcept override { released_.wait(); }
    void complete() noexcept override {}
    void release() noexcept { released_.count_down(); }

private:
    std::latch released_{1};
};

// listen_tcp binds ::1 when the host has IPv6, where 127.0.0.1 would not reach it.
std::string loopback_of(int fd) {
    int domain = 0;
    socklen_t len = sizeof domain;
    EXPECT_EQ(::getsockopt(fd, SOL_SOCKET, SO_DOMAIN, &domain, &len), 0);
    return domain == AF_INET6 ? "::1" : "127.0.0.1";
}

// The first cell of a statement's result, or the error it ended with.
std::unique_ptr<Query> query(const Statement& statement, std::optional<Answer>& out) {
    return std::make_unique<Query>(statement, [&out](Outcome outcome) noexcept {
        if (outcome) {
            out = Answer{outcome->get(0, 0).value_or("")};
        } else {
            out = std::unexpected(outcome.error());
        }
    });
}

// Opens a transaction, writes a row, and stops without committing.
class LeavesTransactionOpen final : public Operation {
public:
    explicit LeavesTransactionOpen(bool& finished) : finished_(finished) {}

    [[nodiscard]] Statement start() noexcept override {
        wrote_ = false;
        return Statement{.sql = "BEGIN", .params = {}};
    }
    [[nodiscard]] std::optional<Statement> next(Outcome outcome) noexcept override {
        if (outcome && !wrote_) {
            wrote_ = true;
            return Statement{.sql = "INSERT INTO probe VALUES (1)", .params = {}};
        }
        finished_ = outcome.has_value();
        return std::nullopt;
    }
    void abandon(DbError /*error*/) noexcept override { finished_ = false; }

private:
    bool& finished_;
    bool wrote_ = false;
};

class PoolTest : public ::testing::TestWithParam<net::ReactorKind> {
protected:
    void SetUp() override {
        auto r = net::make_reactor(GetParam(), clock, 4096);
        ASSERT_TRUE(r) << "reactor: " << std::strerror(r.error());
        reactor = std::move(*r);
        auto p = net::OffloadPool::create(*reactor, 1);
        ASSERT_TRUE(p);
        offload = std::move(*p);
    }

    void TearDown() override {
        // A pool requires the offload pool stopped before it goes.
        offload.reset();
        pool.reset();
        reactor.reset();
        db.reset();
    }

    void start(const std::string& conninfo, std::size_t connections = 4,
               core::Millis request_timeout = kTimeout) {
        auto made = Pool::create(*reactor, *offload,
                                 PoolConfig{.conninfo = conninfo,
                                            .application_name = "ulw-test",
                                            .connections = connections,
                                            .connect_timeout = kTimeout,
                                            .request_timeout = request_timeout});
        ASSERT_TRUE(made) << made.error();
        pool = std::move(*made);
    }

    Answer ask(Sql sql, const Params& params = {}) {
        std::optional<Answer> answer;
        pool->submit(query(Statement{.sql = sql, .params = params}, answer));
        if (!ulw::test::pump_until(*reactor, [&] { return answer.has_value(); })) {
            ADD_FAILURE() << "no answer";
            return std::unexpected(DbError::Rejected);
        }
        return *answer;
    }

    // Drives the loop for `span`, returning the longest single iteration.
    Clock::duration pump_measuring(std::chrono::milliseconds span) {
        Clock::duration longest{};
        const auto deadline = Clock::now() + span;
        while (Clock::now() < deadline) {
            const auto before = Clock::now();
            reactor->run_once(core::Millis{5});
            longest = std::max(longest, Clock::now() - before);
        }
        return longest;
    }

    os::SystemClock clock;
    std::unique_ptr<ScratchDatabase> db;
    std::unique_ptr<net::IReactor> reactor;
    std::unique_ptr<net::OffloadPool> offload;
    std::unique_ptr<Pool> pool;
};

TEST_P(PoolTest, AnswersOnALaterIterationNeverInsideSubmit) {
    ScratchDatabase::open(db);
    if (IsSkipped() || HasFatalFailure()) {
        return;
    }
    ASSERT_NO_FATAL_FAILURE(start(db->conninfo()));
    std::optional<Answer> answer;
    pool->submit(query(Statement{.sql = "SELECT 6 * 7", .params = {}}, answer));
    EXPECT_FALSE(answer);
    ASSERT_TRUE(ulw::test::pump_until(*reactor, [&] { return answer.has_value(); }));
    EXPECT_EQ(*answer, Answer{"42"});
}

TEST_P(PoolTest, StartsOperationsInSubmissionOrder) {
    ScratchDatabase::open(db);
    if (IsSkipped() || HasFatalFailure()) {
        return;
    }
    ASSERT_NO_FATAL_FAILURE(start(db->conninfo(), 1));
    constexpr std::size_t kQueries = 5;
    std::vector<std::string> order;
    for (std::size_t i = 0; i < kQueries; ++i) {
        pool->submit(std::make_unique<Query>(
            Statement{.sql = "SELECT $1::bigint",
                      .params = Params{}.add_int(static_cast<std::int64_t>(i))},
            [&order](Outcome outcome) noexcept {
                order.emplace_back(outcome ? outcome->get(0, 0).value_or("") : "error");
            }));
    }
    ASSERT_TRUE(ulw::test::pump_until(*reactor, [&] { return order.size() == kQueries; }));
    EXPECT_EQ(order, (std::vector<std::string>{"0", "1", "2", "3", "4"}));
}

TEST_P(PoolTest, RollsBackATransactionAnOperationLeftOpen) {
    ScratchDatabase::open(db);
    if (IsSkipped() || HasFatalFailure()) {
        return;
    }
    auto conn = db->session();
    ASSERT_TRUE(conn.exec("CREATE TABLE probe (x integer)"));
    ASSERT_NO_FATAL_FAILURE(start(db->conninfo(), 1));
    bool finished = false;
    pool->submit(std::make_unique<LeavesTransactionOpen>(finished));
    ASSERT_TRUE(ulw::test::pump_until(*reactor, [&] { return finished; }));
    // The only session answers next, so it must have rolled the insert back first.
    EXPECT_EQ(ask("SELECT count(*) FROM probe"), Answer{"0"});
    EXPECT_EQ(pool->sessions_lost(), 0U);
}

TEST_P(PoolTest, RerunsAnOperationThatLostASerializationRace) {
    ScratchDatabase::open(db);
    if (IsSkipped() || HasFatalFailure()) {
        return;
    }
    auto conn = db->session();
    // A sequence is not rolled back with the failed attempt, so it counts attempts.
    ASSERT_TRUE(conn.run_script(R"sql(
        CREATE SEQUENCE tries;
        CREATE FUNCTION flaky(succeed_on bigint) RETURNS bigint LANGUAGE plpgsql AS $$
        DECLARE n bigint := nextval('tries');
        BEGIN
            IF n < succeed_on THEN
                RAISE EXCEPTION 'lost a race' USING ERRCODE = '40001';
            END IF;
            RETURN n;
        END $$;)sql"));
    ASSERT_NO_FATAL_FAILURE(start(db->conninfo(), 1));
    EXPECT_EQ(ask("SELECT flaky($1)", Params{}.add_int(3)), Answer{"3"});

    ASSERT_TRUE(conn.exec("SELECT setval('tries', 1, false)"));
    EXPECT_EQ(ask("SELECT flaky($1)", Params{}.add_int(100)), std::unexpected(DbError::Retry));
    EXPECT_EQ(scalar(conn, "SELECT last_value FROM tries"), "3");
}

TEST_P(PoolTest, ServerStatementTimeoutEndsAStatementButKeepsTheSession) {
    ScratchDatabase::open(db);
    if (IsSkipped() || HasFatalFailure()) {
        return;
    }
    ASSERT_NO_FATAL_FAILURE(start(db->conninfo(), 1));
    // Connected first: time spent queued for a session would eat into the server's head start.
    ASSERT_EQ(ask("SELECT 1"), Answer{"1"});
    EXPECT_EQ(ask("SELECT pg_sleep(30)"), std::unexpected(DbError::Timeout));
    EXPECT_EQ(ask("SELECT 1"), Answer{"1"});
    EXPECT_EQ(pool->sessions_lost(), 0U);
}

TEST_P(PoolTest, ResolvesHostNamesOnTheOffloadPool) {
    ScratchDatabase::open(db);
    if (IsSkipped() || HasFatalFailure()) {
        return;
    }
    OffloadBlocker blocker;
    offload->submit(blocker);
    // A later keyword wins in a key=value string.
    ASSERT_NO_FATAL_FAILURE(start(db->conninfo() + " host=localhost", 1));
    // The lookup waits behind the blocker, so no session can connect and the query sits out its
    // deadline. Had libpq resolved the name itself, on the loop, the query would have answered.
    EXPECT_EQ(ask("SELECT 1"), std::unexpected(DbError::Timeout));
    blocker.release();
    EXPECT_EQ(ask("SELECT 1"), Answer{"1"});
}

TEST_P(PoolTest, ServerThatNeverAnswersNeverStallsTheLoop) {
    // Accepts connections (the kernel does, into the backlog) and never says a word.
    auto listener = net::listen_tcp({.port = 0, .loopback_only = true});
    ASSERT_TRUE(listener);
    const auto port = net::local_port(listener->get());
    ASSERT_TRUE(port);
    ASSERT_NO_FATAL_FAILURE(start(std::format("host={} port={} user=nobody dbname=none",
                                              loopback_of(listener->get()), *port)));

    Ticker ticker(*reactor);
    ticker.start();
    std::optional<Answer> answer;
    pool->submit(query(Statement{.sql = "SELECT 1", .params = {}}, answer));
    const auto longest = pump_measuring(kObserved);

    ASSERT_TRUE(answer);
    EXPECT_FALSE(*answer);
    EXPECT_LT(longest, kLongestAcceptableSlice);
    EXPECT_GE(ticker.ticks(), kMinimumTicks);
}

TEST_P(PoolTest, RefusedConnectionsFailOperationsWithoutWaitingOutTheirDeadline) {
    // Bound and closed again: nothing listens there.
    std::uint16_t port = 0;
    std::string host;
    {
        auto probe = net::listen_tcp({.port = 0, .loopback_only = true});
        ASSERT_TRUE(probe);
        port = *net::local_port(probe->get());
        host = loopback_of(probe->get());
    }
    ASSERT_NO_FATAL_FAILURE(
        start(std::format("host={} port={} user=x dbname=x", host, port), 4, core::Millis{30000}));
    const auto before = Clock::now();
    EXPECT_EQ(ask("SELECT 1"), std::unexpected(DbError::ConnectionLost));
    EXPECT_LT(Clock::now() - before, std::chrono::seconds(5));
}

TEST_P(PoolTest, ReconnectsAfterTheServerEndsItsSessions) {
    ScratchDatabase::open(db);
    if (IsSkipped() || HasFatalFailure()) {
        return;
    }
    ASSERT_NO_FATAL_FAILURE(start(db->conninfo()));
    ASSERT_EQ(ask("SELECT 1"), Answer{"1"});
    auto conn = db->session();
    ASSERT_NE(scalar(conn, "SELECT count(pg_terminate_backend(pid)) FROM pg_stat_activity "
                           "WHERE datname = current_database() AND application_name = 'ulw-test'"),
              "0");
    Answer answer = std::unexpected(DbError::ConnectionLost);
    ASSERT_TRUE(ulw::test::pump_until(*reactor, [&] {
        answer = ask("SELECT 1");
        return answer.has_value();
    }));
    EXPECT_EQ(answer, Answer{"1"});
    EXPECT_GT(pool->sessions_lost(), 0U);
}

class Notifications final : public infra::postgres::INotificationSink {
public:
    void on_listening() noexcept override { ++listening; }
    void on_notification(std::string_view payload) noexcept override {
        payloads.emplace_back(payload);
    }

    int listening = 0;
    std::vector<std::string> payloads;
};

TEST_P(PoolTest, AListeningSessionHandsOverNotificationsAndReportsEveryRestart) {
    ScratchDatabase::open(db);
    if (IsSkipped() || HasFatalFailure()) {
        return;
    }
    Notifications sink;
    auto made = Pool::create(*reactor, *offload,
                             PoolConfig{.conninfo = db->conninfo(),
                                        .application_name = "ulw-test",
                                        .connections = 1,
                                        .connect_timeout = kTimeout,
                                        .request_timeout = kTimeout,
                                        .listen = Sql{"LISTEN probe"},
                                        .notifications = &sink});
    ASSERT_TRUE(made) << made.error();
    pool = std::move(*made);
    ASSERT_TRUE(ulw::test::pump_until(*reactor, [&] { return sink.listening == 1; }));

    auto conn = db->session();
    ASSERT_TRUE(conn.exec("SELECT pg_notify('probe', 'one')"));
    ASSERT_TRUE(ulw::test::pump_until(*reactor, [&] { return sink.payloads.size() == 1; }));

    // What is sent while no session listens is lost, which is why the restart is reported.
    ASSERT_EQ(scalar(conn, "SELECT count(pg_terminate_backend(pid)) FROM pg_stat_activity "
                           "WHERE datname = current_database() AND application_name = 'ulw-test'"),
              "1");
    ASSERT_TRUE(ulw::test::pump_until(*reactor, [&] { return sink.listening == 2; }));
    ASSERT_TRUE(conn.exec("SELECT pg_notify('probe', 'two')"));
    ASSERT_TRUE(ulw::test::pump_until(*reactor, [&] { return sink.payloads.size() == 2; }));
    EXPECT_EQ(sink.payloads, (std::vector<std::string>{"one", "two"}));
    // The listening session still takes ordinary work.
    EXPECT_EQ(ask("SELECT 1"), Answer{"1"});
}

TEST_P(PoolTest, PausedDatabaseNeverStallsTheLoop) {
    ScratchDatabase::open(db);
    if (IsSkipped() || HasFatalFailure()) {
        return;
    }
    const auto container = ulw::test::postgres_container();
    if (!container) {
        GTEST_SKIP() << "docker cannot pause the Postgres container (set ULW_TEST_PG_CONTAINER)";
    }
    ASSERT_NO_FATAL_FAILURE(start(db->conninfo()));
    ASSERT_EQ(ask("SELECT 1"), Answer{"1"});
    {
        const ulw::test::PausedServer paused(*container);
        ASSERT_TRUE(paused.paused());
        Ticker ticker(*reactor);
        ticker.start();
        std::array<std::optional<Answer>, 3> answers;
        for (auto& answer : answers) {
            pool->submit(query(Statement{.sql = "SELECT 1", .params = {}}, answer));
        }
        const auto longest = pump_measuring(kObserved);
        for (const auto& answer : answers) {
            ASSERT_TRUE(answer);
            EXPECT_EQ(*answer, std::unexpected(DbError::Timeout));
        }
        EXPECT_LT(longest, kLongestAcceptableSlice);
        EXPECT_GE(ticker.ticks(), kMinimumTicks);
    }
    Answer answer = std::unexpected(DbError::ConnectionLost);
    ASSERT_TRUE(ulw::test::pump_until(
        *reactor,
        [&] {
            answer = ask("SELECT 1");
            return answer.has_value();
        },
        std::chrono::seconds(30)));
    EXPECT_EQ(answer, Answer{"1"});
}

INSTANTIATE_TEST_SUITE_P(Reactors, PoolTest,
                         ::testing::Values(net::ReactorKind::IoUring, net::ReactorKind::Epoll),
                         ulw::test::reactor_name);

} // namespace
