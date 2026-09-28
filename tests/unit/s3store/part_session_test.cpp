// White-box tests of the streaming session against a scripted S3 peer.
#include "core/ports/storage.hpp"
#include "infra/curl/multi.hpp"
#include "infra/s3util/credentials.hpp"
#include "infra/s3util/profile.hpp"
#include "net/reactor_factory.hpp"
#include "os/system_clock.hpp"

#include "endpoint.hpp"
#include "part_session.hpp"
#include "support/fake_clock.hpp"
#include "support/http_test_server.hpp"
#include "support/reactor_harness.hpp"

#include <algorithm>
#include <functional>
#include <gtest/gtest.h>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

namespace {

using core::ports::IngestId;
using core::ports::IngestState;
using core::ports::StorageError;
using infra::storage::s3::PartSession;
using ulw::test::kKiB;
using ulw::test::pump_until;
using ulw::test::Reply;
using ulw::test::ServedRequest;

// Larger than the session's ring, as every real part size is.
constexpr std::uint64_t kChunk = 96 * kKiB;

struct Observer final : core::ports::IIngestObserver {
    int calls = 0;
    // Set by the test around session calls; a call to the observer inside one is a bug.
    bool inside_session_call = false;
    bool reentered = false;
    void on_ingest_progress() noexcept override {
        ++calls;
        reentered = reentered || inside_session_call;
    }
};

class PartSessionTest : public ::testing::TestWithParam<net::ReactorKind> {
protected:
    PartSessionTest()
        : server([this](const ServedRequest& r) {
              const std::scoped_lock lock(respond_mutex);
              return respond(r);
          }) {}

    void SetUp() override {
        auto r = net::make_reactor(GetParam(), system_clock, 1024);
        ASSERT_TRUE(r);
        reactor = std::move(*r);
        auto m = infra::curl::Multi::create(*reactor);
        ASSERT_TRUE(m);
        multi = std::move(*m);
        const auto profile = *infra::s3util::S3Profile::minio(server.base_url());
        endpoint = std::make_unique<infra::storage::s3::Endpoint>(
            *infra::s3util::Bucket::make(profile, "ingest"), profile, credentials, clock);
    }
    void TearDown() override {
        multi.reset();
        reactor.reset();
    }

    void respond_with(std::function<Reply(const ServedRequest&)> f) {
        const std::scoped_lock lock(respond_mutex);
        respond = std::move(f);
    }

    [[nodiscard]] std::unique_ptr<PartSession> open(std::uint64_t total, std::uint64_t offset,
                                                    Observer& observer) {
        const IngestId id{.key = *core::StorageKey::parse("videos/v1/raw"),
                          .backend_ref = "upload-7",
                          .total_bytes = total,
                          .chunk_size = kChunk};
        return std::make_unique<PartSession>(
            infra::storage::s3::SessionDeps{
                .reactor = *reactor, .multi = *multi, .endpoint = *endpoint, .pages = pages},
            id, offset, observer);
    }

    static std::size_t write(PartSession& s, Observer& o, std::span<const std::byte> bytes) {
        o.inside_session_call = true;
        const std::size_t n = s.write(bytes);
        o.inside_session_call = false;
        return n;
    }

    static void finish(PartSession& s, Observer& o) {
        o.inside_session_call = true;
        s.finish();
        o.inside_session_call = false;
    }

    // Everything, turning the loop whenever the session pushes back.
    bool write_all(PartSession& s, Observer& o, std::span<const std::byte> bytes) {
        while (!bytes.empty()) {
            const std::size_t n = write(s, o, bytes);
            bytes = bytes.subspan(n);
            if (n == 0) {
                if (s.state() != IngestState::Open) {
                    return false;
                }
                const int before = o.calls;
                if (!pump_until(*reactor, [&] { return o.calls != before; })) {
                    return false;
                }
            }
        }
        return true;
    }

    bool settled(const PartSession& s) {
        return pump_until(*reactor, [&] {
            return s.state() == IngestState::Committed || s.state() == IngestState::Failed;
        });
    }

    [[nodiscard]] std::vector<ServedRequest> parts(bool complete) const {
        std::vector<ServedRequest> out;
        for (auto& r : server.requests()) {
            if (r.complete == complete) {
                out.push_back(r);
            }
        }
        return out;
    }

    os::SystemClock system_clock;
    ulw::test::FakeClock clock;
    infra::s3util::StaticCredentialProvider credentials{
        *infra::s3util::Credentials::make("AKIDEXAMPLE", infra::s3util::SecretString("secret"))};
    std::mutex respond_mutex;
    std::function<Reply(const ServedRequest&)> respond = [](const ServedRequest& r) {
        return Reply{
            .status = 200,
            .headers = {{"ETag",
                         "\"etag-" + std::string(r.query("partNumber").value_or("")) + "\""}},
            .body = {}};
    };
    ulw::test::HttpTestServer server;
    std::unique_ptr<net::IReactor> reactor;
    std::unique_ptr<infra::curl::Multi> multi;
    std::unique_ptr<infra::storage::s3::Endpoint> endpoint;
    infra::storage::s3::PageCount pages;
};

TEST_P(PartSessionTest, EachPartIsOneUnsignedPutOfExactlyItsLength) {
    const auto data = ulw::test::pattern((2 * kChunk) + (10 * kKiB));
    Observer obs;
    auto session = open(data.size(), 0, obs);
    ASSERT_TRUE(write_all(*session, obs, data));
    finish(*session, obs);
    ASSERT_TRUE(settled(*session));
    EXPECT_EQ(session->state(), IngestState::Committed);
    EXPECT_EQ(session->durable_offset(), data.size());
    EXPECT_FALSE(session->error().has_value());
    EXPECT_FALSE(obs.reentered);

    const auto sent = parts(true);
    ASSERT_EQ(sent.size(), 3U);
    std::uint64_t at = 0;
    for (std::size_t i = 0; i < sent.size(); ++i) {
        const auto& p = sent[i];
        const std::uint64_t len = std::min<std::uint64_t>(kChunk, data.size() - at);
        EXPECT_EQ(p.method, "PUT");
        EXPECT_EQ(p.path(), "/ingest/videos/v1/raw");
        EXPECT_EQ(p.query("partNumber"), std::to_string(i + 1));
        EXPECT_EQ(p.query("uploadId"), "upload-7");
        EXPECT_EQ(p.header("content-length"), std::to_string(len));
        EXPECT_EQ(p.header("x-amz-content-sha256"), "UNSIGNED-PAYLOAD");
        EXPECT_TRUE(p.header("authorization").has_value());
        EXPECT_EQ(p.header("expect"), std::nullopt);
        EXPECT_TRUE(
            std::ranges::equal(std::as_bytes(std::span(p.body)), std::span(data).subspan(at, len)));
        at += len;
    }
}

TEST_P(PartSessionTest, FullRingIsBackpressureAndTheObserverHearsWhenItDrains) {
    const auto data = ulw::test::pattern(2 * kChunk);
    Observer obs;
    auto session = open(data.size(), 0, obs);
    std::size_t accepted = 0;
    while (const std::size_t n = write(*session, obs, std::span(data).subspan(accepted))) {
        accepted += n;
    }
    EXPECT_EQ(accepted, PartSession::kBufferBytes);
    EXPECT_FALSE(session->wants_more());
    EXPECT_EQ(session->state(), IngestState::Open);
    EXPECT_FALSE(session->error().has_value());

    ASSERT_TRUE(pump_until(*reactor, [&] { return obs.calls > 0; }));
    EXPECT_TRUE(session->wants_more());
    EXPECT_GT(write(*session, obs, std::span(data).subspan(accepted)), 0U);
    EXPECT_FALSE(obs.reentered);
}

TEST_P(PartSessionTest, FailedPartFailsTheSessionAndKeepsTheDurableOffset) {
    respond_with([](const ServedRequest& r) {
        if (r.query("partNumber") == "2") {
            return Reply{.status = 500,
                         .headers = {},
                         .body = "<Error><Code>InternalError</Code><Message>x</Message></Error>"};
        }
        return Reply{.status = 200, .headers = {{"ETag", "\"e\""}}, .body = {}};
    });
    const auto data = ulw::test::pattern(3 * kChunk);
    Observer obs;
    auto session = open(data.size(), 0, obs);
    static_cast<void>(write_all(*session, obs, data));
    ASSERT_TRUE(settled(*session));
    EXPECT_EQ(session->state(), IngestState::Failed);
    EXPECT_EQ(session->error(), StorageError::Transient);
    EXPECT_EQ(session->durable_offset(), kChunk);
    EXPECT_EQ(session->write(data), 0U);
    // No third part: a failed session sends nothing more.
    EXPECT_EQ(server.request_count(), 2U);
}

TEST_P(PartSessionTest, RefusedCredentialsSurfaceAsUnauthorized) {
    respond_with([](const ServedRequest&) {
        return Reply{
            .status = 403, .headers = {}, .body = "<Error><Code>AccessDenied</Code></Error>"};
    });
    const auto data = ulw::test::pattern(kChunk);
    Observer obs;
    auto session = open(data.size(), 0, obs);
    static_cast<void>(write_all(*session, obs, data));
    finish(*session, obs);
    ASSERT_TRUE(settled(*session));
    EXPECT_EQ(session->error(), StorageError::Unauthorized);
    EXPECT_EQ(session->durable_offset(), 0U);
}

TEST_P(PartSessionTest, ABadSignatureOnAPartIsPermanentAndCounted) {
    respond_with([](const ServedRequest&) {
        return Reply{.status = 403,
                     .headers = {},
                     .body = "<Error><Code>SignatureDoesNotMatch</Code></Error>"};
    });
    const auto data = ulw::test::pattern(kChunk);
    Observer obs;
    auto session = open(data.size(), 0, obs);
    static_cast<void>(write_all(*session, obs, data));
    ASSERT_TRUE(settled(*session));
    EXPECT_EQ(session->error(), StorageError::Permanent);
    EXPECT_EQ(pages.value(), 1U);
}

TEST_P(PartSessionTest, FinishMidPartCancelsThatPartAndKeepsOnlyWholeOnes) {
    const auto data = ulw::test::pattern(3 * kChunk);
    Observer obs;
    auto session = open(data.size(), 0, obs);
    ASSERT_TRUE(write_all(*session, obs, std::span(data).first(kChunk + (40 * kKiB))));
    // Part 2 has to be on the wire, not merely started, for the cut to be visible to S3.
    ASSERT_TRUE(pump_until(*reactor, [&] {
        return session->durable_offset() == kChunk && server.body_bytes_in_progress() == 40 * kKiB;
    }));
    finish(*session, obs);
    ASSERT_TRUE(settled(*session));
    EXPECT_EQ(session->state(), IngestState::Committed);
    EXPECT_EQ(session->durable_offset(), kChunk);
    EXPECT_FALSE(obs.reentered);
    ASSERT_TRUE(pump_until(*reactor, [&] { return server.request_count() == 2; }));
    const auto complete = parts(true);
    ASSERT_EQ(complete.size(), 1U);
    EXPECT_EQ(complete[0].query("partNumber"), "1");
    const auto cut = parts(false);
    ASSERT_EQ(cut.size(), 1U);
    EXPECT_EQ(cut[0].query("partNumber"), "2");
    EXPECT_LT(cut[0].body.size(), kChunk);
}

TEST_P(PartSessionTest, FinalPartAlreadyBufferedAtFinishIsStillSent) {
    const auto data = ulw::test::pattern(kChunk + 100);
    Observer obs;
    auto session = open(data.size(), 0, obs);
    ASSERT_TRUE(write_all(*session, obs, data));
    // The last 100 bytes sit in the ring behind part 1, which may not even have finished.
    finish(*session, obs);
    ASSERT_TRUE(settled(*session));
    EXPECT_EQ(session->state(), IngestState::Committed);
    EXPECT_EQ(session->durable_offset(), data.size());
    const auto sent = parts(true);
    ASSERT_EQ(sent.size(), 2U);
    EXPECT_EQ(sent[1].query("partNumber"), "2");
    EXPECT_EQ(sent[1].body.size(), 100U);
}

TEST_P(PartSessionTest, ResumingAtAPartBoundaryStartsWithThatPart) {
    const auto data = ulw::test::pattern(2 * kChunk);
    Observer obs;
    auto session = open(data.size(), kChunk, obs);
    EXPECT_EQ(session->durable_offset(), kChunk);
    ASSERT_TRUE(write_all(*session, obs, std::span(data).subspan(kChunk)));
    finish(*session, obs);
    ASSERT_TRUE(settled(*session));
    EXPECT_EQ(session->durable_offset(), data.size());
    const auto sent = parts(true);
    ASSERT_EQ(sent.size(), 1U);
    EXPECT_EQ(sent[0].query("partNumber"), "2");
}

TEST_P(PartSessionTest, AbortCancelsTheUploadAndSilencesTheObserver) {
    const auto data = ulw::test::pattern(kChunk);
    Observer obs;
    auto session = open(data.size(), 0, obs);
    ASSERT_EQ(write(*session, obs, std::span(data).first(1000)), 1000U);
    ASSERT_TRUE(pump_until(*reactor, [&] { return server.body_bytes_in_progress() == 1000; }));
    session->abort();
    session->abort();
    const int calls = obs.calls;
    ASSERT_TRUE(pump_until(*reactor, [&] { return server.request_count() == 1; }));
    ulw::test::pump_for(*reactor, std::chrono::milliseconds(50));
    EXPECT_EQ(obs.calls, calls);
    EXPECT_EQ(session->state(), IngestState::Failed);
    EXPECT_FALSE(server.requests()[0].complete);
}

TEST_P(PartSessionTest, SessionAtTheEndOfTheObjectTakesNothingAndFinishesAtOnce) {
    Observer obs;
    auto session = open(kChunk, kChunk, obs);
    EXPECT_FALSE(session->wants_more());
    EXPECT_EQ(write(*session, obs, ulw::test::pattern(10)), 0U);
    finish(*session, obs);
    EXPECT_EQ(obs.calls, 0);
    ASSERT_TRUE(settled(*session));
    ASSERT_TRUE(pump_until(*reactor, [&] { return obs.calls > 0; }));
    EXPECT_EQ(session->state(), IngestState::Committed);
    EXPECT_EQ(server.request_count(), 0U);
}

INSTANTIATE_TEST_SUITE_P(Reactors, PartSessionTest,
                         ::testing::Values(net::ReactorKind::IoUring, net::ReactorKind::Epoll),
                         ulw::test::reactor_name);

} // namespace
