// KubernetesPackagers against a scripted API server: what it asks for, in what order, with
// which credentials, and how it reads the answers.
#include "core/util/json.hpp"
#include "infra/curl/multi.hpp"
#include "infra/packagers/kubernetes_packagers.hpp"
#include "net/offload_pool.hpp"
#include "net/reactor_factory.hpp"
#include "os/system_clock.hpp"

#include "support/fake_clock.hpp"
#include "support/http_test_server.hpp"
#include "support/reactor_harness.hpp"
#include "support/temp_dir.hpp"

#include <fstream>
#include <gtest/gtest.h>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {

using core::ports::PackagerError;
using core::ports::PackagerState;
using infra::packagers::check_job_template;
using infra::packagers::fill_job_template;
using infra::packagers::JobValues;
using infra::packagers::KubernetesConfig;
using infra::packagers::KubernetesPackagers;
using ulw::test::HttpTestServer;
using ulw::test::Reply;
using ulw::test::ServedRequest;

constexpr std::string_view kStream = "0192f3a4-0000-7000-8000-0000000000aa";

JobValues job_values() {
    return JobValues{.namespace_name = "apps-test",
                     .image_tag = "abc123@sha256:00ff",
                     .pull_policy = "IfNotPresent",
                     .storage = "minio",
                     .r2_account_id = "",
                     .s3_endpoint = "http://minio:9000",
                     .bucket = "ulw-test",
                     .packager_secret = "live-packager-secrets"};
}
constexpr std::string_view kTemplate =
    "apiVersion: batch/v1\n"
    "kind: Job\n"
    "metadata:\n"
    "  name: ${ULW_STREAM_ID}\n"
    "  namespace: ${LIVE_NAMESPACE}\n"
    "spec:\n"
    "  template:\n"
    "    spec:\n"
    "      containers:\n"
    "        - name: packager\n"
    "          image: ghcr.io/example/ulw-live-packager:${LIVE_PACKAGER_IMAGE_TAG}\n"
    "          imagePullPolicy: ${IMAGE_PULL_POLICY}\n"
    "          env:\n"
    "            - name: ULW_STORAGE\n"
    "              value: ${STORAGE}\n"
    "            - name: ULW_R2_ACCOUNT_ID\n"
    "              value: \"${R2_ACCOUNT_ID}\"\n"
    "            - name: ULW_S3_ENDPOINT\n"
    "              value: \"${S3_ENDPOINT}\"\n"
    "            - name: ULW_BUCKET\n"
    "              value: ${BUCKET}\n"
    "            - name: ULW_DATABASE_URL\n"
    "              valueFrom:\n"
    "                secretKeyRef:\n"
    "                  name: ${LIVE_PACKAGER_SECRET}\n"
    "                  key: ULW_DATABASE_URL\n"
    "            - name: ULW_STREAM_OWNER\n"
    "              value: \"${ULW_STREAM_OWNER}\"\n"
    "            - name: ULW_LIVE_INGEST_HOST\n"
    "              value: $(POD_IP)\n";

// The API server's side: Secrets and Jobs by name, and each Job's status as the test sets it.
class FakeApiServer {
public:
    FakeApiServer() : server_([this](const ServedRequest& r) { return answer(r); }) {}

    [[nodiscard]] std::string url() const { return server_.base_url(); }
    [[nodiscard]] std::vector<ServedRequest> requests() const { return server_.requests(); }

    void set_status(std::string_view job, std::string status) {
        const std::scoped_lock lock(mutex_);
        status_[std::string(job)] = std::move(status);
    }
    void fail_with(std::optional<int> status, std::string message = {}) {
        const std::scoped_lock lock(mutex_);
        fail_ = status;
        fail_message_ = std::move(message);
    }

private:
    Reply answer(const ServedRequest& r) {
        const std::scoped_lock lock(mutex_);
        if (fail_) {
            std::string body = R"({"kind":"Status","code":)" + std::to_string(*fail_);
            if (!fail_message_.empty()) {
                body += R"(,"message":")" + fail_message_ + '"';
            }
            return json(*fail_, body + "}");
        }
        const std::string path(r.path());
        const std::string secrets = "/api/v1/namespaces/apps-test/secrets";
        const std::string jobs = "/apis/batch/v1/namespaces/apps-test/jobs";
        if (r.method == "POST" && path == secrets) {
            const auto doc = core::json::parse(r.body);
            const std::string name(*doc->find("metadata")->find("name")->as_string());
            return json(secrets_.insert(name).second ? 201 : 409, "{}");
        }
        if (r.method == "POST" && path == jobs) {
            // The stream's name is the Job's: the line after "name: ".
            const std::size_t at = r.body.find("name: ") + 6;
            const std::string name = r.body.substr(at, r.body.find('\n', at) - at);
            const bool made = !uids_.contains(name);
            if (made) {
                uids_[name] = "uid-" + std::to_string(uids_.size() + 1);
                status_.try_emplace(name, "{}");
            }
            return json(made ? 201 : 409, made
                                              ? R"({"metadata":{"name":")" + name + R"(","uid":")" +
                                                    uids_[name] + R"("},"status":{}})"
                                              : R"({"kind":"Status","reason":"AlreadyExists"})");
        }
        if (r.method == "GET" && path.starts_with(jobs + "/")) {
            const std::string name = path.substr(jobs.size() + 1);
            if (!uids_.contains(name)) {
                return json(404, R"({"kind":"Status","reason":"NotFound"})");
            }
            return json(200, R"({"metadata":{"name":")" + name + R"(","uid":")" + uids_[name] +
                                 R"("},"status":)" + status_[name] + "}");
        }
        return json(400, "{}");
    }

    static Reply json(int status, std::string body) {
        return Reply{.status = status,
                     .headers = {{"Content-Type", "application/json"}},
                     .body = std::move(body)};
    }

    std::mutex mutex_;
    std::set<std::string> secrets_;
    std::map<std::string, std::string> uids_;
    std::map<std::string, std::string> status_;
    std::optional<int> fail_;
    std::string fail_message_;
    HttpTestServer server_;
};

class KubernetesPackagersTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto r = net::make_reactor(net::ReactorKind::Epoll, reactor_clock, 256);
        ASSERT_TRUE(r);
        reactor = std::move(*r);
        auto m = infra::curl::Multi::create(*reactor);
        ASSERT_TRUE(m);
        multi = std::move(*m);
        auto p = net::OffloadPool::create(*reactor, 1);
        ASSERT_TRUE(p);
        pool = std::move(*p);
        write_token("fake-token-testtest123\n");
        auto made = KubernetesPackagers::create(*reactor, *multi, *pool, clock, config());
        ASSERT_TRUE(made) << made.error();
        packagers = std::move(*made);
    }

    void TearDown() override {
        pool.reset();
        packagers.reset();
        multi.reset();
        reactor.reset();
    }

    KubernetesConfig config() const {
        return KubernetesConfig{.api_url = api.url() + "/",
                                .token_file = (dir.path() / "token").string(),
                                .ca_file = "",
                                .job_template = std::string(kTemplate),
                                .job = job_values()};
    }

    void write_token(std::string_view text) const {
        std::ofstream(dir.path() / "token", std::ios::trunc) << text;
    }

    std::expected<void, PackagerError> start(std::string_view owner = "auth0|alice") {
        std::optional<std::expected<void, PackagerError>> r;
        packagers->start({.stream = *core::LiveStreamId::parse(kStream),
                          .owner = *core::UserId::parse(owner),
                          .passphrase = "fake-passphrase-testtest123"},
                         [&](auto x) noexcept { r = x; });
        EXPECT_FALSE(r.has_value()) << "answered inside the call";
        EXPECT_TRUE(ulw::test::pump_until(*reactor, [&] { return r.has_value(); }));
        return r.value_or(std::unexpected(PackagerError::Unavailable));
    }

    std::expected<PackagerState, PackagerError> state(std::string_view stream = kStream) {
        std::optional<std::expected<PackagerState, PackagerError>> r;
        packagers->state(*core::LiveStreamId::parse(stream), [&](auto x) noexcept { r = x; });
        EXPECT_FALSE(r.has_value()) << "answered inside the call";
        EXPECT_TRUE(ulw::test::pump_until(*reactor, [&] { return r.has_value(); }));
        return r.value_or(std::unexpected(PackagerError::Unavailable));
    }

    ulw::test::TempDir dir{"ulw-k8s"};
    FakeApiServer api;
    os::SystemClock reactor_clock;
    ulw::test::FakeClock clock;
    std::unique_ptr<net::IReactor> reactor;
    std::unique_ptr<infra::curl::Multi> multi;
    std::unique_ptr<net::OffloadPool> pool;
    std::unique_ptr<KubernetesPackagers> packagers;
};

TEST_F(KubernetesPackagersTest, StartingMakesTheJobThenItsSecretOwnedByIt) {
    ASSERT_TRUE(start());
    const auto requests = api.requests();
    ASSERT_EQ(requests.size(), 2U);
    for (const ServedRequest& r : requests) {
        EXPECT_EQ(r.header("authorization"), "Bearer fake-token-testtest123");
        EXPECT_EQ(r.method, "POST");
    }
    const std::string id(kStream);
    // The Job, the template filled in: Kubernetes' own $(POD_IP) left for it to expand.
    EXPECT_EQ(requests[0].path(), "/apis/batch/v1/namespaces/apps-test/jobs");
    EXPECT_EQ(requests[0].header("content-type"), "application/yaml");
    EXPECT_EQ(requests[0].body, fill_job_template(kTemplate, job_values(), id, "auth0|alice"));
    EXPECT_NE(requests[0].body.find("value: \"auth0|alice\""), std::string::npos);
    EXPECT_NE(requests[0].body.find("$(POD_IP)"), std::string::npos);
    // The stream's Secret, holding its passphrase, owned by the Job from its creation.
    EXPECT_EQ(requests[1].path(), "/api/v1/namespaces/apps-test/secrets");
    EXPECT_EQ(requests[1].header("content-type"), "application/json");
    const auto secret = core::json::parse(requests[1].body);
    ASSERT_TRUE(secret);
    EXPECT_EQ(secret->find("metadata")->find("name")->as_string(), "live-packager-" + id);
    EXPECT_EQ(secret->find("type")->as_string(), "Opaque");
    EXPECT_EQ(secret->find("stringData")->find("ULW_LIVE_SRT_PASSPHRASE")->as_string(),
              "fake-passphrase-testtest123");
    const auto* owners = secret->find("metadata")->find("ownerReferences")->as_array();
    ASSERT_EQ(owners->size(), 1U);
    EXPECT_EQ((*owners)[0].find("kind")->as_string(), "Job");
    EXPECT_EQ((*owners)[0].find("name")->as_string(), id);
    EXPECT_EQ((*owners)[0].find("uid")->as_string(), "uid-1");
}

TEST_F(KubernetesPackagersTest, AStartRepeatedFinishesTheFirstOnesWork) {
    ASSERT_TRUE(start());
    ASSERT_TRUE(start());
    const auto requests = api.requests();
    ASSERT_EQ(requests.size(), 5U);
    // The Job made already: read for its uid, and its Secret, made already too, is done.
    EXPECT_EQ(requests[3].method, "GET");
    EXPECT_EQ(requests[4].method, "POST");
    EXPECT_NE(requests[4].body.find("uid-1"), std::string::npos);
}

TEST_F(KubernetesPackagersTest, AJobTheServerRefusesLeavesNoSecret) {
    api.fail_with(422);
    EXPECT_EQ(start(), std::unexpected(PackagerError::Refused));
    const auto requests = api.requests();
    ASSERT_EQ(requests.size(), 1U);
    EXPECT_EQ(requests[0].path(), "/apis/batch/v1/namespaces/apps-test/jobs");
}

TEST_F(KubernetesPackagersTest, TheJobsStatusSaysHowThePackagerStands) {
    EXPECT_EQ(state(), PackagerState::Absent);
    ASSERT_TRUE(start());
    EXPECT_EQ(state(), PackagerState::Starting);
    api.set_status(kStream, R"({"active":1,"ready":0})");
    EXPECT_EQ(state(), PackagerState::Starting);
    api.set_status(kStream, R"({"active":1,"ready":1})");
    EXPECT_EQ(state(), PackagerState::Ready);
    // A cluster too old to count ready pods: a running one is the best sign.
    api.set_status(kStream, R"({"active":1})");
    EXPECT_EQ(state(), PackagerState::Ready);
    api.set_status(kStream, R"({"succeeded":1})");
    EXPECT_EQ(state(), PackagerState::Finished);
    api.set_status(kStream, R"({"conditions":[{"type":"SuccessCriteriaMet","status":"True"},)"
                            R"({"type":"Complete","status":"True"}]})");
    EXPECT_EQ(state(), PackagerState::Finished);
    api.set_status(kStream, R"({"failed":7,"conditions":[{"type":"Failed","status":"True"}]})");
    EXPECT_EQ(state(), PackagerState::Failed);
    api.set_status(kStream, R"({"conditions":[{"type":"Failed","status":"False"}],"ready":1})");
    EXPECT_EQ(state(), PackagerState::Ready);
    api.set_status(kStream, R"("not an object")");
    EXPECT_EQ(state(), PackagerState::Starting);
}

TEST_F(KubernetesPackagersTest, FailuresAreUnavailableOrRefusedByWhatARetryCouldDo) {
    api.fail_with(503);
    EXPECT_EQ(start(), std::unexpected(PackagerError::Unavailable));
    EXPECT_EQ(state(), std::unexpected(PackagerError::Unavailable));
    api.fail_with(429);
    EXPECT_EQ(state(), std::unexpected(PackagerError::Unavailable));
    api.fail_with(403);
    EXPECT_EQ(start(), std::unexpected(PackagerError::Refused));
    EXPECT_EQ(state(), std::unexpected(PackagerError::Refused));
    api.fail_with(422);
    EXPECT_EQ(start(), std::unexpected(PackagerError::Refused));
    api.fail_with(std::nullopt);
    ASSERT_TRUE(start());
}

TEST_F(KubernetesPackagersTest, ASpentQuotaIsFullNotRefused) {
    // As the API server's quota admission words it.
    api.fail_with(403, R"(jobs.batch \"x\" is forbidden: exceeded quota: live-packagers, )"
                       R"(requested: count/jobs.batch=1, used: count/jobs.batch=300, )"
                       R"(limited: count/jobs.batch=300)");
    EXPECT_EQ(start(), std::unexpected(PackagerError::Full));
    api.fail_with(403, R"(jobs.batch is forbidden: User \"x\" cannot create resource)");
    EXPECT_EQ(start(), std::unexpected(PackagerError::Refused));
}

TEST_F(KubernetesPackagersTest, PackagersDestroyedWhileTheTokenIsReadLeaveThePoolSafe) {
    std::optional<std::expected<PackagerState, PackagerError>> r;
    packagers->state(*core::LiveStreamId::parse(kStream), [&](auto x) noexcept { r = x; });
    ASSERT_EQ(pool->in_flight(), 1U);
    // The token's job is on the pool; its owner goes before it completes.
    packagers.reset();
    EXPECT_TRUE(ulw::test::pump_until(*reactor, [&] { return pool->in_flight() == 0; }));
    EXPECT_FALSE(r.has_value());
}

TEST_F(KubernetesPackagersTest, AnUnreachableServerIsUnavailable) {
    pool.reset();
    packagers.reset();
    auto p = net::OffloadPool::create(*reactor, 1);
    ASSERT_TRUE(p);
    pool = std::move(*p);
    KubernetesConfig c = config();
    // A port nothing listens on: the server's own, closed by now.
    c.api_url = "http://127.0.0.1:1";
    auto made = KubernetesPackagers::create(*reactor, *multi, *pool, clock, c);
    ASSERT_TRUE(made);
    packagers = std::move(*made);
    EXPECT_EQ(state(), std::unexpected(PackagerError::Unavailable));
}

TEST_F(KubernetesPackagersTest, TheTokenIsReadAgainOnceAMinuteOld) {
    ASSERT_TRUE(start());
    write_token("fake-token-rotated-testtest123");
    ASSERT_TRUE(state());
    EXPECT_EQ(api.requests().back().header("authorization"), "Bearer fake-token-testtest123");
    clock.advance(core::Millis{61'000});
    ASSERT_TRUE(state());
    EXPECT_EQ(api.requests().back().header("authorization"),
              "Bearer fake-token-rotated-testtest123");
}

TEST_F(KubernetesPackagersTest, NoTokenIsTheConfigurationsFault) {
    std::filesystem::remove(dir.path() / "token");
    clock.advance(core::Millis{61'000});
    EXPECT_EQ(state(), std::unexpected(PackagerError::Refused));
    EXPECT_TRUE(api.requests().empty());
}

TEST(KubernetesJobTemplate, OnlyTheConfigKeysAndTheStreamAreFilled) {
    EXPECT_TRUE(check_job_template(kTemplate));
    EXPECT_FALSE(check_job_template("name: fixed\n"));
    const auto unknown = check_job_template("name: ${ULW_STREAM_ID}\nimage: ${ULW_OTHER}\n");
    ASSERT_FALSE(unknown);
    EXPECT_NE(unknown.error().find("${ULW_OTHER}"), std::string::npos);
    EXPECT_FALSE(check_job_template("name: ${ULW_STREAM_ID} ${"));
    // The names a by-hand start fills from config.env, so both make the same Job.
    EXPECT_EQ(fill_job_template("a ${LIVE_NAMESPACE} b ${LIVE_PACKAGER_IMAGE_TAG} c "
                                "${ULW_STREAM_ID} d \"${ULW_STREAM_OWNER}\" $(POD_IP) "
                                "${IMAGE_PULL_POLICY} ${STORAGE} \"${R2_ACCOUNT_ID}\" "
                                "\"${S3_ENDPOINT}\" ${BUCKET} ${LIVE_PACKAGER_SECRET} ${",
                                job_values(), "s1", "u|1"),
              "a apps-test b abc123@sha256:00ff c s1 d \"u|1\" $(POD_IP) IfNotPresent minio \"\" "
              "\"http://minio:9000\" ulw-test live-packager-secrets ${");
    // A name the template no longer uses is refused like any other.
    EXPECT_FALSE(check_job_template("name: ${ULW_STREAM_ID}\nnamespace: ${ULW_NAMESPACE}\n"));
}

TEST(KubernetesPackagersConfig, RefusesWhatCouldNotWork) {
    os::SystemClock reactor_clock;
    auto reactor = std::move(*net::make_reactor(net::ReactorKind::Epoll, reactor_clock, 64));
    auto multi = std::move(*infra::curl::Multi::create(*reactor));
    auto pool = std::move(*net::OffloadPool::create(*reactor, 1));
    const KubernetesConfig good{.api_url = "https://kubernetes.default.svc",
                                .token_file = "/nonexistent",
                                .ca_file = "/nonexistent",
                                .job_template = std::string(kTemplate),
                                .job = job_values()};
    EXPECT_TRUE(KubernetesPackagers::create(*reactor, *multi, *pool, reactor_clock, good));
    auto bad = good;
    bad.api_url = "kubernetes.default.svc";
    EXPECT_FALSE(KubernetesPackagers::create(*reactor, *multi, *pool, reactor_clock, bad));
    // Each value goes into a YAML scalar: nothing that would end one, or name what it should not.
    const std::vector<std::pair<std::string JobValues::*, std::string>> refused{
        {&JobValues::namespace_name, "Apps"},
        {&JobValues::image_tag, "main\nimage: evil"},
        {&JobValues::pull_policy, "Sometimes"},
        {&JobValues::storage, "fs"},
        {&JobValues::r2_account_id, "0123\nx: y"},
        {&JobValues::s3_endpoint, "http://minio:9000\"\n"},
        {&JobValues::bucket, "Media Bucket"},
        {&JobValues::packager_secret, "video-gateway-secrets\n"},
    };
    for (const auto& [field, value] : refused) {
        bad = good;
        bad.job.*field = value;
        EXPECT_FALSE(KubernetesPackagers::create(*reactor, *multi, *pool, reactor_clock, bad))
            << value;
    }
    bad = good;
    bad.job_template = "kind: Job\n";
    EXPECT_FALSE(KubernetesPackagers::create(*reactor, *multi, *pool, reactor_clock, bad));
    pool.reset();
}

} // namespace
