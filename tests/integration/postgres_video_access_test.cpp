#include "core/models/ids.hpp"
#include "core/models/video.hpp"
#include "core/models/video_access.hpp"
#include "core/models/visibility.hpp"
#include "infra/postgres/upload_catalog.hpp"
#include "net/offload_pool.hpp"
#include "net/reactor_factory.hpp"
#include "os/system_clock.hpp"
#include "os/system_random.hpp"

#include "postgres_harness.hpp"
#include "support/reactor_harness.hpp"

#include <chrono>
#include <cstddef>
#include <cstring>
#include <gtest/gtest.h>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

// The catalog's side of ADR-0097 on a real database: the one-statement access read, the owner's
// visibility checked against chat_members, the service's grants, and migration 0016's schema.
namespace {

using core::ports::CatalogError;
using core::ports::CatalogResult;
using core::ports::GrantPage;
using core::ports::VideoView;
using infra::postgres::CatalogConfig;
using infra::postgres::Params;
using infra::postgres::PgUploadCatalog;
using ulw::test::Reply;
using ulw::test::scalar;
using ulw::test::ScratchDatabase;

constexpr std::string_view kRoom = "0192f3c4-7a1b-7c2d-8e3f-0123456789ab";
constexpr std::string_view kOtherRoom = "0192f3c4-7a1b-7c2d-8e3f-0123456789ac";

core::UserId user(std::string_view name) {
    return *core::UserId::parse(name);
}

core::RoomId room(std::string_view id = kRoom) {
    return *core::RoomId::parse(id);
}

class VideoAccessTest : public ::testing::TestWithParam<net::ReactorKind> {
protected:
    void SetUp() override {
        ScratchDatabase::open(db);
        if (IsSkipped() || HasFatalFailure()) {
            return;
        }
        auto r = net::make_reactor(GetParam(), clock, 4096);
        ASSERT_TRUE(r) << "reactor: " << std::strerror(r.error());
        reactor = std::move(*r);
        auto p = net::OffloadPool::create(*reactor, 1);
        ASSERT_TRUE(p);
        offload = std::move(*p);
        auto made =
            PgUploadCatalog::create(*reactor, *offload, CatalogConfig{.conninfo = db->conninfo()});
        ASSERT_TRUE(made) << made.error();
        catalog = std::move(*made);
    }

    void TearDown() override {
        offload.reset();
        catalog.reset();
        reactor.reset();
        db.reset();
    }

    template <class T, class Start> CatalogResult<T> call(Start start) {
        Reply<T> reply;
        start(reply.callback());
        return ulw::test::wait(*reactor, reply);
    }

    // A video of `owner`'s in `state`, as the upload and the worker leave it.
    core::VideoId add_video(std::string_view state = "ready", std::string_view owner = "alice") {
        const auto id = core::VideoId::generate(clock, random);
        auto conn = db->session();
        EXPECT_TRUE(conn.exec("INSERT INTO videos (id, owner_id, title, state, version, "
                              "duration_ms) VALUES ($1, $2, 'trip', $3::text::video_state, 2, "
                              "CASE WHEN $3 = 'ready' THEN 5000 END)",
                              Params{}.add_uuid(id.uuid()).add_text(owner).add_text(state)));
        return id;
    }

    void list_member(std::string_view who, std::string_view in = kRoom) {
        auto conn = db->session();
        ASSERT_TRUE(conn.exec("INSERT INTO chat_rooms (room_id, kind) VALUES ($1, 'group_chat') "
                              "ON CONFLICT (room_id) DO NOTHING",
                              Params{}.add_uuid(room(in).uuid())));
        ASSERT_TRUE(conn.exec("INSERT INTO chat_members (room_id, user_id) VALUES ($1, $2)",
                              Params{}.add_uuid(room(in).uuid()).add_text(who)));
    }

    void unlist_member(std::string_view who, std::string_view in = kRoom) {
        auto conn = db->session();
        ASSERT_TRUE(conn.exec("DELETE FROM chat_members WHERE room_id = $1 AND user_id = $2",
                              Params{}.add_uuid(room(in).uuid()).add_text(who)));
    }

    CatalogResult<VideoView> view(const core::VideoId& id, std::string_view viewer) {
        return call<VideoView>(
            [&](auto done) { catalog->find_video_for(id, user(viewer), std::move(done)); });
    }
    core::VideoAccess access(const core::VideoId& id, std::string_view viewer) {
        const auto v = view(id, viewer);
        EXPECT_TRUE(v) << core::ports::to_string(v.error());
        return v ? core::access_of(v->video, user(viewer), v->viewer) : core::VideoAccess::None;
    }
    CatalogResult<core::VideoRecord> set(const core::VideoId& id, std::string_view owner,
                                         const core::Visibility& visibility) {
        return call<core::VideoRecord>([&](auto done) {
            catalog->set_visibility(id, user(owner), visibility, std::move(done));
        });
    }
    CatalogResult<void> grant(const core::VideoId& id, std::string_view who) {
        return call<void>(
            [&](auto done) { catalog->grant_access(id, user(who), std::move(done)); });
    }
    CatalogResult<void> revoke(const core::VideoId& id, std::string_view who) {
        return call<void>(
            [&](auto done) { catalog->revoke_access(id, user(who), std::move(done)); });
    }
    CatalogResult<GrantPage> grants(const core::VideoId& id, std::optional<std::string_view> after,
                                    std::size_t limit) {
        std::optional<core::UserId> cursor;
        if (after) {
            cursor = user(*after);
        }
        return call<GrantPage>(
            [&](auto done) { catalog->list_grants(id, cursor, limit, std::move(done)); });
    }

    os::SystemClock clock;
    os::SystemRandom random;
    std::unique_ptr<ScratchDatabase> db;
    std::unique_ptr<net::IReactor> reactor;
    std::unique_ptr<net::OffloadPool> offload;
    std::unique_ptr<PgUploadCatalog> catalog;
};

TEST_P(VideoAccessTest, EveryExistingAndNewVideoIsPrivate) {
    const auto id = add_video();
    const auto v = view(id, "alice");
    ASSERT_TRUE(v) << core::ports::to_string(v.error());
    EXPECT_EQ(v->video.visibility, core::Visibility{});
    EXPECT_FALSE(v->viewer.room_member);
    EXPECT_FALSE(v->viewer.granted);
    EXPECT_EQ(access(id, "alice"), core::VideoAccess::Owner);
    EXPECT_EQ(access(id, "bob"), core::VideoAccess::None);
    const auto plain =
        call<core::VideoRecord>([&](auto done) { catalog->find_video(id, std::move(done)); });
    ASSERT_TRUE(plain);
    EXPECT_EQ(plain->visibility, core::Visibility{});
}

TEST_P(VideoAccessTest, AnUnknownOrDeletedVideoIsNotFound) {
    const auto id = core::VideoId::generate(clock, random);
    EXPECT_EQ(view(id, "alice").error(), CatalogError::NotFound);
    const auto deleted = add_video();
    auto conn = db->session();
    ASSERT_TRUE(conn.exec("UPDATE videos SET deleted_at = now() WHERE id = $1",
                          Params{}.add_uuid(deleted.uuid())));
    EXPECT_EQ(view(deleted, "alice").error(), CatalogError::NotFound);
    EXPECT_EQ(set(deleted, "alice", core::Visibility::unlisted()).error(), CatalogError::NotFound);
    EXPECT_EQ(grant(deleted, "bob").error(), CatalogError::NotFound);
    EXPECT_EQ(revoke(deleted, "bob").error(), CatalogError::NotFound);
    EXPECT_EQ(grants(deleted, std::nullopt, 10).error(), CatalogError::NotFound);
}

TEST_P(VideoAccessTest, TheOwnerSharesWithAnyoneByMakingItUnlisted) {
    const auto id = add_video();
    const auto set_to = set(id, "alice", core::Visibility::unlisted());
    ASSERT_TRUE(set_to) << core::ports::to_string(set_to.error());
    EXPECT_EQ(set_to->visibility, core::Visibility::unlisted());
    EXPECT_EQ(set_to->state, core::VideoState::Ready);
    EXPECT_EQ(access(id, "bob"), core::VideoAccess::Viewer);
    ASSERT_TRUE(set(id, "alice", {}));
    EXPECT_EQ(access(id, "bob"), core::VideoAccess::None);
}

TEST_P(VideoAccessTest, OnlyTheOwnerSetsItAndAnyoneElseIsToldItDoesNotExist) {
    const auto id = add_video();
    EXPECT_EQ(set(id, "bob", core::Visibility::unlisted()).error(), CatalogError::NotFound);
    EXPECT_EQ(view(id, "alice")->video.visibility, core::Visibility{});
}

TEST_P(VideoAccessTest, ARoomMustBeOneTheOwnerIsListedIn) {
    const auto id = add_video();
    list_member("alice", kOtherRoom);
    EXPECT_EQ(set(id, "alice", core::Visibility::room(room())).error(), CatalogError::Forbidden);
    EXPECT_EQ(view(id, "alice")->video.visibility, core::Visibility{});
    list_member("alice");
    const auto shared = set(id, "alice", core::Visibility::room(room()));
    ASSERT_TRUE(shared) << core::ports::to_string(shared.error());
    EXPECT_EQ(shared->visibility, core::Visibility::room(room()));
    auto conn = db->session();
    EXPECT_EQ(scalar(conn, "SELECT visibility || ' ' || visibility_room FROM videos WHERE id = $1",
                     Params{}.add_uuid(id.uuid())),
              "room " + std::string(kRoom));
}

TEST_P(VideoAccessTest, ARoomsCurrentMembersSeeItAndARemovedOneLosesIt) {
    const auto id = add_video();
    list_member("alice");
    list_member("bob");
    list_member("carol", kOtherRoom);
    ASSERT_TRUE(set(id, "alice", core::Visibility::room(room())));
    const auto bob = view(id, "bob");
    ASSERT_TRUE(bob);
    EXPECT_TRUE(bob->viewer.room_member);
    EXPECT_FALSE(bob->viewer.granted);
    EXPECT_EQ(access(id, "bob"), core::VideoAccess::Viewer);
    EXPECT_EQ(access(id, "carol"), core::VideoAccess::None);
    EXPECT_EQ(access(id, "dave"), core::VideoAccess::None);

    unlist_member("bob");
    EXPECT_EQ(access(id, "bob"), core::VideoAccess::None);
    // Moving a member to another room by an UPDATE is a removal too.
    list_member("bob");
    EXPECT_EQ(access(id, "bob"), core::VideoAccess::Viewer);
    auto conn = db->session();
    ASSERT_TRUE(conn.exec("UPDATE chat_members SET room_id = $1 WHERE room_id = $2 AND user_id = "
                          "'bob'",
                          Params{}.add_uuid(room(kOtherRoom).uuid()).add_uuid(room().uuid())));
    EXPECT_EQ(access(id, "bob"), core::VideoAccess::None);
    // The owner leaving the room takes nothing from the members, nor from the owner.
    unlist_member("alice");
    EXPECT_EQ(access(id, "alice"), core::VideoAccess::Owner);
}

TEST_P(VideoAccessTest, AGrantOpensAPrivateVideoUntilItIsRevoked) {
    const auto id = add_video();
    ASSERT_TRUE(grant(id, "auth0|bob"));
    EXPECT_TRUE(view(id, "auth0|bob")->viewer.granted);
    EXPECT_EQ(access(id, "auth0|bob"), core::VideoAccess::Viewer);
    EXPECT_EQ(access(id, "carol"), core::VideoAccess::None);
    // Both are idempotent.
    EXPECT_TRUE(grant(id, "auth0|bob"));
    ASSERT_TRUE(revoke(id, "auth0|bob"));
    EXPECT_EQ(access(id, "auth0|bob"), core::VideoAccess::None);
    EXPECT_TRUE(revoke(id, "auth0|bob"));
    auto conn = db->session();
    EXPECT_EQ(scalar(conn, "SELECT count(*) FROM video_grants"), "0");
}

TEST_P(VideoAccessTest, AnUploadInProgressStaysTheUploadersDespiteGrantsAndRooms) {
    for (const std::string_view state : {"init", "uploading"}) {
        const auto id = add_video(state);
        ASSERT_TRUE(grant(id, "bob"));
        EXPECT_EQ(access(id, "bob"), core::VideoAccess::None) << state;
        EXPECT_EQ(access(id, "alice"), core::VideoAccess::Owner) << state;
    }
}

TEST_P(VideoAccessTest, GrantsAreListedInPagesInBytewiseOrder) {
    const auto id = add_video();
    const auto lonely = add_video();
    // "B" sorts before "a" bytewise, whatever the database's collation says.
    for (const std::string_view who : {"carol", "a", "B", "auth0|dave"}) {
        ASSERT_TRUE(grant(id, who)) << who;
    }
    const auto first = grants(id, std::nullopt, 2);
    ASSERT_TRUE(first) << core::ports::to_string(first.error());
    ASSERT_EQ(first->grants.size(), 2U);
    EXPECT_EQ(first->grants[0].user.view(), "B");
    EXPECT_EQ(first->grants[1].user.view(), "a");
    EXPECT_TRUE(first->more);
    const auto age = clock.wall_now() - first->grants[0].granted_at;
    EXPECT_LT(age, std::chrono::minutes(5));
    EXPECT_GT(age, -std::chrono::minutes(5));

    const auto rest = grants(id, "a", 2);
    ASSERT_TRUE(rest);
    ASSERT_EQ(rest->grants.size(), 2U);
    EXPECT_EQ(rest->grants[0].user.view(), "auth0|dave");
    EXPECT_EQ(rest->grants[1].user.view(), "carol");
    EXPECT_FALSE(rest->more);
    const auto none = grants(id, "carol", 2);
    ASSERT_TRUE(none);
    EXPECT_TRUE(none->grants.empty());
    EXPECT_FALSE(none->more);
    const auto empty = grants(lonely, std::nullopt, 10);
    ASSERT_TRUE(empty);
    EXPECT_TRUE(empty->grants.empty());
}

TEST_P(VideoAccessTest, AVideosGrantsGoWithIt) {
    const auto id = add_video();
    ASSERT_TRUE(grant(id, "bob"));
    auto conn = db->session();
    ASSERT_TRUE(conn.exec("DELETE FROM videos WHERE id = $1", Params{}.add_uuid(id.uuid())));
    EXPECT_EQ(scalar(conn, "SELECT count(*) FROM video_grants"), "0");
}

TEST_P(VideoAccessTest, ALiveRecordingInsertedWithoutTheColumnsIsPrivate) {
    // The live packager's role inserts the recording's video naming only its own columns
    // (RUNBOOK, ulw_live), so the defaults apply.
    const auto id = core::VideoId::generate(clock, random);
    auto conn = db->session();
    ASSERT_TRUE(conn.exec("INSERT INTO videos (id, owner_id, title, state, version) "
                          "VALUES ($1, 'broadcaster', 'live', 'processing', 1)",
                          Params{}.add_uuid(id.uuid())));
    const auto recording = view(id, "broadcaster");
    ASSERT_TRUE(recording) << core::ports::to_string(recording.error());
    EXPECT_EQ(recording->video.visibility, core::Visibility{});
    EXPECT_EQ(access(id, "bob"), core::VideoAccess::None);
}

TEST_P(VideoAccessTest, TheSchemaRefusesWhatTheApiNeverWrites) {
    const auto id = add_video();
    auto conn = db->session();
    const Params video = Params{}.add_uuid(id.uuid());
    EXPECT_FALSE(conn.exec("UPDATE videos SET visibility = 'public' WHERE id = $1", video));
    EXPECT_FALSE(conn.exec("UPDATE videos SET visibility = 'room' WHERE id = $1", video));
    EXPECT_FALSE(
        conn.exec("UPDATE videos SET visibility_room = gen_random_uuid() WHERE id = $1", video));
    EXPECT_FALSE(
        conn.exec("INSERT INTO video_grants (video_id, user_id) VALUES ($1, 'a b')", video));
    EXPECT_FALSE(conn.exec("INSERT INTO video_grants (video_id, user_id) VALUES ($1, '')", video));
    EXPECT_FALSE(conn.exec("INSERT INTO video_grants (video_id, user_id) VALUES "
                           "(gen_random_uuid(), 'bob')"));
    // A row the API could not have written reads as corrupt rather than as private.
    ASSERT_TRUE(conn.exec("ALTER TABLE videos DROP CONSTRAINT videos_visibility_room"));
    ASSERT_TRUE(
        conn.exec("UPDATE videos SET visibility_room = gen_random_uuid() WHERE id = $1", video));
    EXPECT_EQ(view(id, "alice").error(), CatalogError::Corrupt);
}

INSTANTIATE_TEST_SUITE_P(Reactors, VideoAccessTest,
                         ::testing::Values(net::ReactorKind::IoUring, net::ReactorKind::Epoll),
                         ulw::test::reactor_name);

} // namespace
