// The stream service's routes (ADR-0092) through a real gateway shard, over fakes of the
// service's store, media server and packagers.
#include "core/util/json.hpp"

#include "gateway_harness.hpp"
#include "support/eventually.hpp"
#include "support/http_client.hpp"

#include <chrono>
#include <gtest/gtest.h>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace {

using core::ports::LiveEnd;
using core::ports::LiveState;
using ulw::test::Backend;
using ulw::test::GatewayOptions;
using ulw::test::GatewayUnderTest;
using ulw::test::HttpClient;
using ulw::test::HttpResponse;
using ulw::test::LiveFakes;

constexpr std::string_view kAlice = "user.alice";
constexpr std::string_view kBob = "user.bob";

GatewayOptions with_streams() {
    GatewayOptions o;
    o.backend = Backend::Fake;
    o.live_streams = true;
    return o;
}

core::json::Value json_of(const HttpResponse& r) {
    auto doc = core::json::parse(r.body);
    EXPECT_TRUE(doc) << r.body;
    return doc ? std::move(*doc) : core::json::Value{};
}

std::string text_at(const core::json::Value& v, std::string_view key) {
    const core::json::Value* f = v.find(key);
    const auto s = f != nullptr ? f->as_string() : std::nullopt;
    return s ? std::string(*s) : std::string{};
}

bool null_at(const core::json::Value& v, std::string_view key) {
    const core::json::Value* f = v.find(key);
    return f != nullptr && f->is_null();
}

class GatewayStreams : public ::testing::Test {
protected:
    HttpResponse send(std::string_view method, const std::string& path, std::string_view token,
                      std::string_view body = {}) {
        HttpClient c(gw.endpoint());
        auto r = c.request(method, path, token, std::as_bytes(std::span(body)));
        EXPECT_TRUE(r);
        return r.value_or(HttpResponse{});
    }

    // A new stream for alice; its id.
    std::string create() {
        const HttpResponse r = send("POST", "/api/v1/live", kAlice);
        EXPECT_EQ(r.status, 201) << r.body;
        return text_at(json_of(r), "id");
    }

    GatewayUnderTest gw{with_streams()};
};

TEST_F(GatewayStreams, AStreamStartsForItsOwnerWithAPublisherTicket) {
    const HttpResponse r = send("POST", "/api/v1/live", kAlice);
    ASSERT_EQ(r.status, 201) << r.body;
    EXPECT_EQ(r.header("content-type"), "application/json");
    EXPECT_EQ(r.header("cache-control"), "no-store");
    const auto doc = json_of(r);
    const std::string id = text_at(doc, "id");
    ASSERT_TRUE(core::LiveStreamId::parse(id));
    EXPECT_EQ(text_at(doc, "state"), "starting");
    EXPECT_EQ(text_at(doc, "playlist"), "/api/v1/live/" + id + "/index.m3u8");
    EXPECT_TRUE(doc.find("created_at")->as_u64().has_value());
    EXPECT_TRUE(null_at(doc, "live_at"));
    EXPECT_TRUE(null_at(doc, "ended_at"));
    EXPECT_TRUE(null_at(doc, "ended_by"));
    EXPECT_TRUE(null_at(doc, "video_id"));
    const core::json::Value* publish = doc.find("publish");
    ASSERT_NE(publish, nullptr);
    EXPECT_EQ(text_at(*publish, "url"), "https://media.example.test/whip/v1");
    EXPECT_EQ(text_at(*publish, "token"), "ticket-1");
    EXPECT_EQ(publish->find("expires_at")->as_u64(), 1767225660U);
    // The passphrase never leaves the server.
    std::string passphrase;
    gw.with_live([&](LiveFakes& f) { passphrase = f.store.rows.at(id).passphrase; });
    EXPECT_EQ(r.body.find(passphrase), std::string::npos);

    // Asking again answers the same stream, with a new ticket.
    const HttpResponse again = send("POST", "/api/v1/live", kAlice);
    ASSERT_EQ(again.status, 200) << again.body;
    EXPECT_EQ(text_at(json_of(again), "id"), id);
    EXPECT_EQ(text_at(*json_of(again).find("publish"), "token"), "ticket-2");
}

TEST_F(GatewayStreams, TicketsGoToTheOwnerAlone) {
    const std::string id = create();
    const HttpResponse r = send("POST", "/api/v1/live/" + id + "/ticket", kAlice);
    ASSERT_EQ(r.status, 200) << r.body;
    EXPECT_EQ(r.header("cache-control"), "no-store");
    const auto doc = json_of(r);
    EXPECT_EQ(text_at(doc, "url"), "https://media.example.test/whip/v1");
    EXPECT_EQ(text_at(doc, "token"), "ticket-2");
    EXPECT_EQ(doc.find("expires_at")->as_u64(), 1767225660U);
    EXPECT_EQ(send("POST", "/api/v1/live/" + id + "/ticket", kBob).status, 404);
    EXPECT_EQ(send("POST", "/api/v1/live/not-a-stream/ticket", kAlice).status, 404);
    EXPECT_EQ(
        send("POST", "/api/v1/live/0192f3a4-0000-7000-8000-000000000001/ticket", kAlice).status,
        404);
}

TEST_F(GatewayStreams, TheOwnerGoesLiveAndAnyoneSeesIt) {
    const std::string id = create();
    EXPECT_EQ(send("POST", "/api/v1/live/" + id + "/start", kBob).status, 404);
    const HttpResponse r = send("POST", "/api/v1/live/" + id + "/start", kAlice);
    ASSERT_EQ(r.status, 200) << r.body;
    EXPECT_EQ(text_at(json_of(r), "state"), "live");
    EXPECT_TRUE(json_of(r).find("live_at")->as_u64().has_value());
    std::size_t relays = 0;
    gw.with_live([&](LiveFakes& f) {
        relays = f.sfu.relays.size();
        EXPECT_EQ(f.packagers.started.size(), 1U);
    });
    EXPECT_EQ(relays, 1U);

    // A viewer sees the stream and where to watch it, and nothing of its recording.
    const HttpResponse seen = send("GET", "/api/v1/live/" + id, kBob);
    ASSERT_EQ(seen.status, 200) << seen.body;
    const auto doc = json_of(seen);
    EXPECT_EQ(text_at(doc, "state"), "live");
    EXPECT_EQ(text_at(doc, "playlist"), "/api/v1/live/" + id + "/index.m3u8");
    EXPECT_EQ(doc.find("video_id"), nullptr);
    EXPECT_EQ(doc.find("publish"), nullptr);
    // The owner sees whether it has a recording yet.
    EXPECT_TRUE(null_at(json_of(send("GET", "/api/v1/live/" + id, kAlice)), "video_id"));
    EXPECT_EQ(send("GET", "/api/v1/live/0192f3a4-0000-7000-8000-000000000001", kBob).status, 404);
    EXPECT_EQ(send("GET", "/api/v1/live/x", kBob).status, 404);
}

TEST_F(GatewayStreams, AnEndedPlaylistIsAnEndedStream) {
    const std::string id = create();
    ASSERT_EQ(send("POST", "/api/v1/live/" + id + "/start", kAlice).status, 200);
    gw.put_object("live/" + id + "/index.m3u8",
                  "#EXTM3U\n#EXT-X-VERSION:7\n#EXT-X-TARGETDURATION:2\n#EXT-X-MEDIA-SEQUENCE:0\n"
                  "#EXT-X-MAP:URI=\"init_0.mp4\"\n#EXTINF:2.000,\nseg_0_0.m4s\n#EXT-X-ENDLIST\n");
    const HttpResponse r = send("GET", "/api/v1/live/" + id, kBob);
    ASSERT_EQ(r.status, 200) << r.body;
    EXPECT_EQ(text_at(json_of(r), "state"), "ended");
    EXPECT_EQ(text_at(json_of(r), "ended_by"), "finished");
    ASSERT_TRUE(ulw::test::eventually([&] {
        bool ended = false;
        gw.with_live([&](LiveFakes& f) {
            ended = f.store.rows.at(id).state == LiveState::Ended &&
                    f.store.rows.at(id).ended_by == LiveEnd::Finished;
        });
        return ended;
    }));
    // A recording, once queued, is the owner's to see.
    const core::VideoId video = *core::VideoId::parse("0192f3a4-0000-7000-8000-0000000000aa");
    gw.with_live([&](LiveFakes& f) { f.store.rows.at(id).recording = video; });
    EXPECT_EQ(text_at(json_of(send("GET", "/api/v1/live/" + id, kAlice)), "video_id"),
              video.to_string());
}

TEST_F(GatewayStreams, ALiveStreamWithoutAPlaylistYetIsStillLive) {
    const std::string id = create();
    ASSERT_EQ(send("POST", "/api/v1/live/" + id + "/start", kAlice).status, 200);
    const HttpResponse r = send("GET", "/api/v1/live/" + id, kBob);
    ASSERT_EQ(r.status, 200) << r.body;
    EXPECT_EQ(text_at(json_of(r), "state"), "live");
}

TEST_F(GatewayStreams, TheOwnerEndsTheStreamAndNothingMoreCanBeDoneWithIt) {
    const std::string id = create();
    EXPECT_EQ(send("POST", "/api/v1/live/" + id + "/end", kBob).status, 404);
    const HttpResponse r = send("POST", "/api/v1/live/" + id + "/end", kAlice);
    ASSERT_EQ(r.status, 200) << r.body;
    const auto doc = json_of(r);
    EXPECT_EQ(text_at(doc, "state"), "ended");
    EXPECT_EQ(text_at(doc, "ended_by"), "owner");
    EXPECT_TRUE(doc.find("ended_at")->as_u64().has_value());
    EXPECT_EQ(send("POST", "/api/v1/live/" + id + "/end", kAlice).status, 200);
    EXPECT_EQ(send("POST", "/api/v1/live/" + id + "/ticket", kAlice).status, 409);
    EXPECT_EQ(send("POST", "/api/v1/live/" + id + "/start", kAlice).status, 409);
    // And the owner can start another.
    EXPECT_EQ(send("POST", "/api/v1/live", kAlice).status, 201);
}

TEST_F(GatewayStreams, FailuresAnswerAsDocumented) {
    gw.with_live([](LiveFakes& f) { f.store.fail = core::ports::LiveStoreError::Unavailable; });
    HttpResponse r = send("POST", "/api/v1/live", kAlice);
    EXPECT_EQ(r.status, 503);
    EXPECT_EQ(r.header("retry-after"), "2");
    gw.with_live([](LiveFakes& f) { f.store.fail = core::ports::LiveStoreError::Full; });
    r = send("POST", "/api/v1/live", kAlice);
    EXPECT_EQ(r.status, 503);
    EXPECT_EQ(r.header("retry-after"), "60");
    gw.with_live([](LiveFakes& f) {
        f.store.fail.reset();
        f.sfu.fail_join = core::ports::MediaError::Refused;
    });
    EXPECT_EQ(send("POST", "/api/v1/live", kAlice).status, 500);
    gw.with_live([](LiveFakes& f) {
        f.sfu.fail_join.reset();
        f.packagers.fail_start = core::ports::PackagerError::Unavailable;
    });
    const std::string id = text_at(json_of(send("POST", "/api/v1/live", kAlice)), "id");
    r = send("POST", "/api/v1/live/" + id + "/start", kAlice);
    EXPECT_EQ(r.status, 503);
    EXPECT_EQ(r.header("retry-after"), "2");
    // The cluster's quota spent is the platform full, as the stream count is.
    gw.with_live([](LiveFakes& f) { f.packagers.fail_start = core::ports::PackagerError::Full; });
    r = send("POST", "/api/v1/live/" + id + "/start", kAlice);
    EXPECT_EQ(r.status, 503);
    EXPECT_EQ(r.header("retry-after"), "60");
}

TEST_F(GatewayStreams, OnlyATokenCarryingTheBroadcasterClaimStartsAStream) {
    // "viewer." tokens verify without what ULW_LIVE_BROADCASTER_CLAIM asks for.
    EXPECT_EQ(send("POST", "/api/v1/live", "viewer.alice").status, 403);
    const std::string id = create();
    // Watching needs nothing of the kind.
    EXPECT_EQ(send("GET", "/api/v1/live/" + id, "viewer.bob").status, 200);
}

TEST_F(GatewayStreams, AUserPastTheHourlyCountIsToldToWait) {
    gw.with_live([](LiveFakes& f) { f.store.fail = core::ports::LiveStoreError::TooMany; });
    const HttpResponse r = send("POST", "/api/v1/live", kAlice);
    EXPECT_EQ(r.status, 429);
    EXPECT_EQ(r.header("retry-after"), "600");
}

TEST_F(GatewayStreams, RequestsCarryNoBodyAndNeedAToken) {
    EXPECT_EQ(send("POST", "/api/v1/live", kAlice, R"({"title":"x"})").status, 400);
    EXPECT_EQ(send("POST", "/api/v1/live", "").status, 401);
    EXPECT_EQ(send("GET", "/api/v1/live/0192f3a4-0000-7000-8000-000000000001", "").status, 401);
    EXPECT_EQ(send("PUT", "/api/v1/live", kAlice).status, 405);
    // A cookie POST from a page that does not say where it comes from is refused unread.
    HttpClient c(gw.endpoint());
    const auto r = c.request("POST", "/api/v1/live", "", {}, {{"Cookie", "auth_token=user.alice"}});
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 403);
}

// Who may watch a stream (ADR-0105).
class GatewayStreamVisibility : public GatewayStreams {
protected:
    static constexpr std::string_view kRoom = "0192f3c4-7a1b-7c2d-8e3f-0123456789ab";

    HttpResponse patch(const std::string& id, std::string_view body,
                       std::string_view token = kAlice) {
        HttpClient c(gw.endpoint());
        auto r = c.request("PATCH", "/api/v1/live/" + id, token, std::as_bytes(std::span(body)),
                           {{"Content-Type", "application/json"}});
        EXPECT_TRUE(r);
        return r.value_or(HttpResponse{});
    }

    // A live stream of alice's with a playlist in the store; its id.
    std::string live_with_playlist() {
        const std::string id = create();
        EXPECT_EQ(send("POST", "/api/v1/live/" + id + "/start", kAlice).status, 200);
        gw.put_object("live/" + id + "/index.m3u8",
                      "#EXTM3U\n#EXT-X-VERSION:7\n#EXT-X-TARGETDURATION:2\n"
                      "#EXT-X-MEDIA-SEQUENCE:0\n#EXT-X-MAP:URI=\"init_0.mp4\"\n"
                      "#EXTINF:2.000,\nseg_0_0.m4s\n");
        return id;
    }

    // What `token` gets from the stream's status and its playlist.
    std::pair<int, int> seen_by(const std::string& id, std::string_view token) {
        return {send("GET", "/api/v1/live/" + id, token).status,
                send("GET", "/api/v1/live/" + id + "/index.m3u8", token).status};
    }

    void member(std::string_view user) {
        gw.with_live([&](LiveFakes& f) { f.store.members.emplace(kRoom, user); });
    }
};

TEST_F(GatewayStreamVisibility, AStreamIsUnlistedUntilItsOwnerSaysOtherwise) {
    const std::string id = live_with_playlist();
    const HttpResponse own = send("GET", "/api/v1/live/" + id, kAlice);
    EXPECT_EQ(text_at(json_of(own), "visibility"), "unlisted");
    const HttpResponse seen = send("GET", "/api/v1/live/" + id, kBob);
    EXPECT_EQ(json_of(seen).find("visibility"), nullptr) << "the owner's to know";
    EXPECT_EQ(seen_by(id, kBob), std::make_pair(200, 200));
}

TEST_F(GatewayStreamVisibility, APrivateStreamIsTheOwnersAloneAndReadsAsNoStream) {
    const std::string id = live_with_playlist();
    const HttpResponse r = patch(id, R"({"visibility":"private"})");
    ASSERT_EQ(r.status, 200) << r.body;
    EXPECT_EQ(r.header("cache-control"), "no-store");
    EXPECT_EQ(text_at(json_of(r), "visibility"), "private");
    EXPECT_EQ(text_at(json_of(r), "state"), "live");
    EXPECT_EQ(seen_by(id, kAlice), std::make_pair(200, 200));
    EXPECT_EQ(seen_by(id, kBob), std::make_pair(404, 404));
    const HttpResponse missing = send("GET", "/api/v1/live/" + id + "/index.m3u8", kBob);
    EXPECT_TRUE(missing.body.empty()) << missing.body;
    // Back to unlisted, and bob is a viewer again at once on this gateway.
    ASSERT_EQ(patch(id, R"({"visibility":"unlisted"})").status, 200);
    EXPECT_EQ(seen_by(id, kBob), std::make_pair(200, 200));
}

TEST_F(GatewayStreamVisibility, ARoomSharesTheStreamWithItsMembersWhileItListsTheOwner) {
    const std::string id = live_with_playlist();
    const std::string room = "room:" + std::string(kRoom);
    const HttpResponse refused = patch(id, R"({"visibility":")" + room + R"("})");
    EXPECT_EQ(refused.status, 403);
    EXPECT_EQ(refused.body, R"({"error":"not_member"})");
    member("alice");
    member("bob");
    const HttpResponse r = patch(id, R"({"visibility":")" + room + R"("})");
    ASSERT_EQ(r.status, 200) << r.body;
    EXPECT_EQ(text_at(json_of(r), "visibility"), room);
    EXPECT_EQ(seen_by(id, kBob), std::make_pair(200, 200));
    EXPECT_EQ(seen_by(id, "user.carol"), std::make_pair(404, 404));
    EXPECT_EQ(seen_by(id, kAlice), std::make_pair(200, 200));
}

TEST_F(GatewayStreamVisibility, OnlyTheOwnerSetsItAndOnlyToOneOfTheThreeForms) {
    const std::string id = create();
    EXPECT_EQ(patch(id, R"({"visibility":"private"})", kBob).status, 404);
    EXPECT_EQ(patch("0192f3a4-0000-7000-8000-000000000001", R"({"visibility":"private"})").status,
              404);
    EXPECT_EQ(patch(id, R"({"visibility":"private"})", "").status, 401);
    for (const std::string_view body :
         {R"({})", R"({"visibility":"public"})", R"({"visibility":"room:nope"})", R"([])",
          R"({"visibility":3})", "not json"}) {
        const HttpResponse r = patch(id, body);
        EXPECT_EQ(r.status, 400) << body;
        EXPECT_EQ(r.body, R"({"error":"bad_visibility"})") << body;
    }
    gw.with_live([&](LiveFakes& f) {
        EXPECT_EQ(f.store.rows.at(id).visibility, core::Visibility::unlisted());
    });
    // An ended stream's still the owner's to hide: its playlist outlives it.
    ASSERT_EQ(send("POST", "/api/v1/live/" + id + "/end", kAlice).status, 200);
    EXPECT_EQ(patch(id, R"({"visibility":"private"})").status, 200);
}

TEST_F(GatewayStreamVisibility, TheOperatorsBackendSetsWhoMayWatchAnyStream) {
    const std::string id = live_with_playlist();
    const auto service_patch = [&](const std::string& stream, std::string_view body,
                                   std::string_view token = "service.backend") {
        HttpClient c(gw.endpoint());
        auto r = c.request("PATCH", "/api/v1/service/live/" + stream, token,
                           std::as_bytes(std::span(body)), {{"Content-Type", "application/json"}});
        EXPECT_TRUE(r);
        return r.value_or(HttpResponse{});
    };
    const HttpResponse hidden = service_patch(id, R"({"visibility":"private"})");
    ASSERT_EQ(hidden.status, 200) << hidden.body;
    EXPECT_EQ(text_at(json_of(hidden), "visibility"), "private") << "seen as its owner sees it";
    EXPECT_EQ(seen_by(id, kBob), std::make_pair(404, 404));
    EXPECT_EQ(seen_by(id, kAlice), std::make_pair(200, 200));
    // A room must list the stream's owner, not the backend.
    const std::string room = R"({"visibility":"room:)" + std::string(kRoom) + R"("})";
    const HttpResponse refused = service_patch(id, room);
    EXPECT_EQ(refused.status, 403);
    EXPECT_EQ(refused.body, R"({"error":"not_member"})");
    member("alice");
    member("bob");
    ASSERT_EQ(service_patch(id, room).status, 200);
    EXPECT_EQ(seen_by(id, kBob), std::make_pair(200, 200));
    // Only the backend's bearer token; a missing stream and a bad body as the video routes say.
    for (const std::string_view token : {kAlice, kBob}) {
        const HttpResponse r = service_patch(id, R"({"visibility":"unlisted"})", token);
        EXPECT_EQ(r.status, 403) << token;
        EXPECT_EQ(r.body, R"({"error":"forbidden"})");
    }
    const HttpResponse missing =
        service_patch("0192f3a4-0000-7000-8000-000000000001", R"({"visibility":"private"})");
    EXPECT_EQ(missing.status, 404);
    EXPECT_EQ(missing.body, R"({"error":"not_found"})");
    EXPECT_EQ(service_patch("not-a-stream", R"({"visibility":"private"})").status, 404);
    const HttpResponse bad = service_patch(id, R"({"visibility":"public"})");
    EXPECT_EQ(bad.status, 400);
    EXPECT_EQ(bad.body, R"({"error":"bad_visibility"})");
}

TEST_F(GatewayStreamVisibility, ManyPlaylistReloadsCostOneRowReadPerFreshnessInterval) {
    const std::string id = live_with_playlist();
    member("alice");
    member("bob");
    ASSERT_EQ(patch(id, R"({"visibility":"room:)" + std::string(kRoom) + R"("})").status, 200);
    std::size_t before = 0;
    gw.with_live([&](LiveFakes& f) { before = f.store.calls; });
    for (int i = 0; i < 20; ++i) {
        ASSERT_EQ(send("GET", "/api/v1/live/" + id + "/index.m3u8", kBob).status, 200);
    }
    gw.with_live([&](LiveFakes& f) {
        // The PATCH left the row known; bob's membership is read once and remembered.
        EXPECT_EQ(f.store.calls - before, 1U);
        EXPECT_EQ(f.store.membership_reads, 1U);
    });
}

TEST_F(GatewayStreamVisibility, AStreamTheServiceDidNotStartIsABroadcastAsBefore) {
    // Started outside the service: no row records who may watch it.
    gw.put_object("live/launch-2026/index.m3u8",
                  "#EXTM3U\n#EXT-X-VERSION:7\n#EXT-X-TARGETDURATION:2\n"
                  "#EXT-X-MEDIA-SEQUENCE:0\n#EXT-X-MAP:URI=\"init_0.mp4\"\n"
                  "#EXTINF:2.000,\nseg_0_0.m4s\n");
    EXPECT_EQ(send("GET", "/api/v1/live/launch-2026/index.m3u8", kBob).status, 200);
    const std::string unknown = "0192f3a4-0000-7000-8000-0000000000ee";
    gw.put_object("live/" + unknown + "/index.m3u8",
                  "#EXTM3U\n#EXT-X-VERSION:7\n#EXT-X-TARGETDURATION:2\n"
                  "#EXT-X-MEDIA-SEQUENCE:0\n#EXT-X-MAP:URI=\"init_0.mp4\"\n"
                  "#EXTINF:2.000,\nseg_0_0.m4s\n");
    EXPECT_EQ(send("GET", "/api/v1/live/" + unknown + "/index.m3u8", kBob).status, 200);
}

TEST_F(GatewayStreamVisibility, AStoreOutageIsARetryForAViewerWhoseAnswerIsNotKnown) {
    const std::string id = create();
    gw.with_live([](LiveFakes& f) { f.store.fail = core::ports::LiveStoreError::Unavailable; });
    const HttpResponse r = send("GET", "/api/v1/live/" + id + "/index.m3u8", kBob);
    EXPECT_EQ(r.status, 503);
    EXPECT_EQ(r.header("retry-after"), "2");
}

TEST_F(GatewayStreamVisibility, AMemberTakenOffTheRoomStopsWatchingOnceTheAnswerIsStale) {
    const std::string id = live_with_playlist();
    member("alice");
    member("bob");
    ASSERT_EQ(patch(id, R"({"visibility":"room:)" + std::string(kRoom) + R"("})").status, 200);
    ASSERT_EQ(seen_by(id, kBob), std::make_pair(200, 200));
    gw.with_live([&](LiveFakes& f) { f.store.members.erase({std::string(kRoom), "bob"}); });
    // The status reads afresh at once; the playlist within watch_fresh (5 s by default).
    EXPECT_EQ(send("GET", "/api/v1/live/" + id, kBob).status, 404);
    ASSERT_TRUE(ulw::test::eventually(
        [&] { return send("GET", "/api/v1/live/" + id + "/index.m3u8", kBob).status == 404; },
        std::chrono::seconds{10}));
}

TEST_F(GatewayStreams, TheServiceIsCounted) {
    const std::string id = create();
    ASSERT_EQ(send("POST", "/api/v1/live/" + id + "/start", kAlice).status, 200);
    ASSERT_EQ(send("POST", "/api/v1/live/" + id + "/end", kAlice).status, 200);
    const std::string metrics = gw.metrics();
    EXPECT_NE(metrics.find("\nlive_streams_created_total 1\n"), std::string::npos) << metrics;
    EXPECT_NE(metrics.find("\nlive_tickets_issued_total 1\n"), std::string::npos);
    EXPECT_NE(metrics.find("\nlive_streams_went_live_total 1\n"), std::string::npos);
    EXPECT_NE(metrics.find("live_streams_ended_total{reason=\"owner\"} 1\n"), std::string::npos);
}

TEST(GatewayWithoutStreams, TheRoutesAnswerNotFound) {
    GatewayOptions o;
    o.backend = Backend::Fake;
    GatewayUnderTest gw(o);
    HttpClient c(gw.endpoint());
    for (const auto& [method, path] : std::map<std::string, std::string>{
             {"POST", "/api/v1/live"},
             {"GET", "/api/v1/live/0192f3a4-0000-7000-8000-000000000001"}}) {
        const auto r = c.request(method, path, kAlice);
        ASSERT_TRUE(r);
        EXPECT_EQ(r->status, 404) << method << " " << path;
    }
    EXPECT_EQ(gw.metrics().find("live_streams_created_total"), std::string::npos);
}

} // namespace
