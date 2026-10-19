#include "infra/postgres/notify_store.hpp"
#include "net/offload_pool.hpp"
#include "net/reactor_factory.hpp"

#include "notify_store_contract.hpp"
#include "postgres_harness.hpp"
#include "support/fake_clock.hpp"
#include "support/fake_random.hpp"
#include "support/reactor_harness.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <gtest/gtest.h>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

using infra::postgres::Params;
using ulw::test::NotifyFactory;
using ulw::test::NotifyHarness;
using ulw::test::NotifyStoreContract;
using ulw::test::ScratchDatabase;

std::string_view kind_text(core::ports::RoomKind kind) noexcept {
    switch (kind) {
    case core::ports::RoomKind::DirectChat:
        return "direct_chat";
    case core::ports::RoomKind::GroupChat:
        return "group_chat";
    case core::ports::RoomKind::StreamLiveChat:
        return "stream_live_chat";
    }
    return "group_chat";
}

// The Postgres registry on a scratch database of its own, with the loop that drives it.
class PgNotifyHarness final : public NotifyHarness {
public:
    static std::unique_ptr<PgNotifyHarness> open(std::size_t connections = 2) {
        auto harness = std::make_unique<PgNotifyHarness>();
        ScratchDatabase::open(harness->db_);
        if (harness->db_ == nullptr) {
            return nullptr;
        }
        auto r = net::make_reactor_with_fallback(net::ReactorKind::IoUring, harness->clock_, 1024);
        if (!r) {
            ADD_FAILURE() << "reactor: " << r.error();
            return nullptr;
        }
        harness->reactor_ = std::move(r->reactor);
        auto offload = net::OffloadPool::create(*harness->reactor_, 1);
        if (!offload) {
            ADD_FAILURE() << "offload pool";
            return nullptr;
        }
        harness->offload_ = std::move(*offload);
        auto made = infra::postgres::PgNotifyStore::create(
            *harness->reactor_, *harness->offload_,
            infra::postgres::NotifyStoreConfig{.conninfo = harness->db_->conninfo(),
                                               .connections = connections,
                                               .request_timeout = core::Millis{10000},
                                               .timing = ulw::test::kContractTiming});
        if (!made) {
            ADD_FAILURE() << made.error();
            return nullptr;
        }
        harness->store_ = std::move(*made);
        return harness;
    }

    PgNotifyHarness() = default;
    ~PgNotifyHarness() override {
        // The store requires the offload pool stopped before it goes.
        offload_.reset();
        store_.reset();
        reactor_.reset();
    }
    PgNotifyHarness(const PgNotifyHarness&) = delete;
    PgNotifyHarness& operator=(const PgNotifyHarness&) = delete;
    PgNotifyHarness(PgNotifyHarness&&) = delete;
    PgNotifyHarness& operator=(PgNotifyHarness&&) = delete;

    net::IReactor& reactor() override { return *reactor_; }
    core::ports::IConnectivityStore& store() override { return *store_; }
    void set_room(const core::RoomId& room, core::ports::RoomKind kind,
                  const std::vector<core::UserId>& members) override {
        auto conn = db_->session();
        const std::string id = room.to_string();
        ASSERT_TRUE(conn.exec("INSERT INTO chat_rooms (room_id, kind) VALUES ($1::text::uuid, $2)",
                              Params{}.add_text(id).add_text(kind_text(kind))));
        for (const core::UserId& member : members) {
            ASSERT_TRUE(
                conn.exec("INSERT INTO chat_members (room_id, user_id) VALUES ($1::text::uuid, $2)",
                          Params{}.add_text(id).add_text(member.view())));
        }
    }
    void wait(core::Millis d) override {
        ulw::test::pump_for(*reactor_, std::chrono::milliseconds(d.count()));
    }
    infra::postgres::PgNotifyStore& pg() { return *store_; }
    void exec(infra::postgres::Sql sql) {
        auto conn = db_->session();
        ASSERT_TRUE(conn.exec(sql, Params{})) << sql.c_str();
    }

private:
    ulw::test::FakeClock clock_;
    std::unique_ptr<ScratchDatabase> db_;
    std::unique_ptr<net::IReactor> reactor_;
    std::unique_ptr<net::OffloadPool> offload_;
    std::unique_ptr<infra::postgres::PgNotifyStore> store_;
};

INSTANTIATE_TEST_SUITE_P(Postgres, NotifyStoreContract,
                         ::testing::Values(NotifyFactory{
                             .name = "postgres",
                             .make = []() -> std::unique_ptr<NotifyHarness> {
                                 return PgNotifyHarness::open();
                             }}),
                         [](const auto& p) { return p.param.name; });

// Many nodes asking about the same message at once, each on a session of its own: every member is
// answered to exactly one of them, as the marks' row locks order the asks.
TEST(PgNotifyStoreTest, ConcurrentAsksTellEachMemberOnce) {
    constexpr std::size_t kAsks = 8;
    auto harness = PgNotifyHarness::open(kAsks);
    if (::testing::Test::IsSkipped() || ::testing::Test::HasFatalFailure()) {
        return;
    }
    ASSERT_NE(harness, nullptr);
    ulw::test::FakeClock clock;
    ulw::test::FakeRandom random;
    const core::RoomId room = core::RoomId::generate(clock, random);
    std::vector<core::UserId> members;
    for (int i = 0; i < 20; ++i) {
        members.push_back(*core::UserId::parse("member-" + std::to_string(i)));
    }
    harness->set_room(room, core::ports::RoomKind::GroupChat, members);
    std::optional<core::ports::NotifyResult<bool>> renewed;
    harness->store().renew(7, [&](auto r) noexcept { renewed = r; });
    ASSERT_TRUE(ulw::test::pump_until(
        harness->reactor(), [&] { return renewed.has_value(); }, std::chrono::seconds(10)));

    std::vector<std::optional<core::ports::NotifyResult<std::vector<core::UserId>>>> answers(kAsks);
    for (std::size_t i = 0; i < kAsks; ++i) {
        harness->store().message_targets(
            room, members[0], [&answers, i](auto r) noexcept { answers[i] = std::move(r); });
    }
    ASSERT_TRUE(ulw::test::pump_until(
        harness->reactor(),
        [&] { return std::ranges::all_of(answers, [](const auto& a) { return a.has_value(); }); },
        std::chrono::seconds(20)));
    std::vector<std::string> told;
    for (const auto& a : answers) {
        ASSERT_TRUE(*a) << to_string(a->error());
        for (const core::UserId& u : **a) {
            told.emplace_back(u.view());
        }
    }
    std::ranges::sort(told);
    std::vector<std::string> expected;
    for (std::size_t i = 1; i < members.size(); ++i) {
        expected.emplace_back(members[i].view());
    }
    std::ranges::sort(expected);
    EXPECT_EQ(told, expected);
}

using VideoEvents = core::ports::NotifyResult<std::vector<core::ports::VideoEvent>>;

VideoEvents take(PgNotifyHarness& harness, std::size_t max = core::ports::kMaxVideoEvents) {
    std::optional<VideoEvents> taken;
    harness.pg().take_video_events(max, [&](VideoEvents r) noexcept { taken = std::move(r); });
    EXPECT_TRUE(ulw::test::pump_until(
        harness.reactor(), [&] { return taken.has_value(); }, std::chrono::seconds(10)));
    return taken.value_or(std::unexpected(core::ports::NotifyError::Unavailable));
}

// ADR-0120: a video that becomes ready or failed is one change, taken once, oldest first.
TEST(PgNotifyStoreTest, AVideoThatSettlesIsTakenOnce) {
    auto harness = PgNotifyHarness::open();
    if (::testing::Test::IsSkipped() || ::testing::Test::HasFatalFailure()) {
        return;
    }
    ASSERT_NE(harness, nullptr);
    harness->exec(R"sql(INSERT INTO videos (id, owner_id, title, state) VALUES
        ('01890a5d-ac96-774b-bcce-b302099a0001', 'alice', 'one', 'processing'),
        ('01890a5d-ac96-774b-bcce-b302099a0002', 'bob', 'two', 'processing'),
        ('01890a5d-ac96-774b-bcce-b302099a0003', 'carol', 'three', 'uploading'))sql");
    // Nothing settled yet: a change of title, or to processing, is not one.
    harness->exec("UPDATE videos SET title = 'renamed'");
    harness->exec("UPDATE videos SET state = 'processing' WHERE owner_id = 'carol'");
    const auto none = take(*harness);
    ASSERT_TRUE(none);
    EXPECT_TRUE(none->empty());

    harness->exec("UPDATE videos SET state = 'ready', duration_ms = 6000, version = 2 "
                  "WHERE owner_id = 'alice'");
    harness->exec("UPDATE videos SET state = 'failed', error_reason = 'bad input', version = 4 "
                  "WHERE owner_id = 'bob'");
    // A deleted video's change is nobody's business.
    harness->exec("UPDATE videos SET deleted_at = now() WHERE owner_id = 'carol'");
    harness->exec("UPDATE videos SET state = 'ready', duration_ms = 1 WHERE owner_id = 'carol'");
    // Setting the same state again is no change.
    harness->exec("UPDATE videos SET state = 'ready' WHERE owner_id = 'alice'");

    const auto first = take(*harness, 1);
    ASSERT_TRUE(first);
    ASSERT_EQ(first->size(), 1U);
    const core::ports::VideoEvent& ready = first->front();
    EXPECT_EQ(ready.video.to_string(), "01890a5d-ac96-774b-bcce-b302099a0001");
    EXPECT_EQ(ready.owner.view(), "alice");
    EXPECT_TRUE(ready.ready);
    EXPECT_EQ(ready.version, 2);
    EXPECT_EQ(ready.duration, core::Millis{6000});
    EXPECT_FALSE(ready.error_reason);
    EXPECT_GT(ready.at_ms, 1'700'000'000'000);

    const auto rest = take(*harness);
    ASSERT_TRUE(rest);
    ASSERT_EQ(rest->size(), 1U);
    EXPECT_EQ(rest->front().owner.view(), "bob");
    EXPECT_FALSE(rest->front().ready);
    EXPECT_EQ(rest->front().error_reason, "bad input");
    EXPECT_FALSE(rest->front().duration);

    const auto after = take(*harness);
    ASSERT_TRUE(after);
    EXPECT_TRUE(after->empty());
    EXPECT_FALSE(take(*harness, 0));
}

// Changes nobody takes are gone an hour on, at the next one.
TEST(PgNotifyStoreTest, ChangesNobodyTakesAreForgottenAnHourOn) {
    auto harness = PgNotifyHarness::open();
    if (::testing::Test::IsSkipped() || ::testing::Test::HasFatalFailure()) {
        return;
    }
    ASSERT_NE(harness, nullptr);
    harness->exec(R"sql(INSERT INTO videos (id, owner_id, title, state) VALUES
        ('01890a5d-ac96-774b-bcce-b302099a0001', 'alice', 'one', 'processing'),
        ('01890a5d-ac96-774b-bcce-b302099a0002', 'bob', 'two', 'processing'))sql");
    harness->exec("UPDATE videos SET state = 'ready', duration_ms = 1 WHERE owner_id = 'alice'");
    harness->exec("UPDATE video_events SET at = now() - interval '61 minutes'");
    harness->exec("UPDATE videos SET state = 'ready', duration_ms = 1 WHERE owner_id = 'bob'");
    const auto taken = take(*harness);
    ASSERT_TRUE(taken);
    ASSERT_EQ(taken->size(), 1U);
    EXPECT_EQ(taken->front().owner.view(), "bob");
}

} // namespace
