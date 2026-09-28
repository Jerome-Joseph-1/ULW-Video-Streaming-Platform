// transcode_worker as a separate process, against a scratch Postgres database and a real
// object store: the M10 acceptance runs, including the workers that die and the ones that
// come back from the dead.
#include "core/models/ids.hpp"
#include "infra/curl/http.hpp"
#include "infra/ffmpeg/transcoder.hpp"
#include "infra/postgres/job_queue.hpp"
#include "infra/s3util/url.hpp"
#include "infra/s3util/xml.hpp"
#include "infra/storage/fs_transfer.hpp"
#include "infra/storage/s3_transfer.hpp"
#include "os/system_clock.hpp"
#include "os/system_random.hpp"

#include "media_clips.hpp"
#include "postgres_harness.hpp"
#include "support/child_process.hpp"
#include "support/live_s3.hpp"
#include "support/temp_dir.hpp"

#include <sys/wait.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <format>
#include <fstream>
#include <grp.h>
#include <gtest/gtest.h>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

namespace fs = std::filesystem;
using infra::postgres::Params;
using std::chrono::seconds;
using ulw::test::ChildProcess;
using ulw::test::scalar;
using ulw::test::ScratchDatabase;
using ulw::test::TempDir;

// Generous: a loaded CI runner may transcode at a fraction of realtime.
constexpr auto kJobPatience = seconds(180);
constexpr auto kExitPatience = seconds(30);

std::string env_or(const char* name, const std::string& fallback) {
    const char* value = std::getenv(name);
    return value == nullptr || *value == '\0' ? fallback : value;
}

// Whole files, /proc ones included, which report a size of 0 and hold NUL bytes.
std::string read_text(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::string text;
    std::array<char, 4096> buffer{};
    while (in.read(buffer.data(), buffer.size()) || in.gcount() > 0) {
        text.append(buffer.data(), static_cast<std::size_t>(in.gcount()));
    }
    return text;
}

// The lines of a media playlist that name files: the segments and the init segment.
std::vector<std::string> referenced_files(const std::string& playlist) {
    std::vector<std::string> files;
    std::size_t at = 0;
    while (at < playlist.size()) {
        const std::size_t eol = playlist.find('\n', at);
        const std::string line = playlist.substr(at, eol - at);
        at = eol == std::string::npos ? playlist.size() : eol + 1;
        constexpr std::string_view kMap = "#EXT-X-MAP:URI=\"";
        if (line.starts_with(kMap)) {
            files.push_back(line.substr(kMap.size(), line.size() - kMap.size() - 1));
        } else if (!line.empty() && !line.starts_with('#')) {
            files.push_back(line);
        }
    }
    return files;
}

// Is any process running whose command line mentions `needle`? The worker's ffmpeg children
// all name their workspace.
bool process_mentions(const std::string& needle) {
    for (const auto& entry : fs::directory_iterator("/proc")) {
        const std::string pid = entry.path().filename().string();
        if (!std::ranges::all_of(pid, [](char c) { return c >= '0' && c <= '9'; })) {
            continue;
        }
        std::string cmdline = read_text(entry.path() / "cmdline");
        std::ranges::replace(cmdline, '\0', ' ');
        const std::string state = read_text(entry.path() / "stat");
        // A zombie still lists its command line until it is reaped.
        const bool zombie = state.find(") Z ") != std::string::npos;
        if (!zombie && cmdline.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

// Deletes what a run left under `prefix` in a bucket that outlives it.
void remove_prefix(const ulw::test::LiveS3& target, const std::string& prefix) {
    const auto bucket = infra::s3util::Bucket::make(target.profile, target.bucket);
    if (!bucket) {
        return;
    }
    const auto listed = ulw::test::send(
        target, infra::curl::Method::Get,
        bucket->root({{.name = "list-type", .value = "2"}, {.name = "prefix", .value = prefix}}));
    if (!listed || listed->status != 200) {
        return;
    }
    const auto page = infra::s3util::parse_list_objects_v2(listed->body);
    if (!page) {
        return;
    }
    for (const std::string& name : page->keys) {
        if (const auto key = core::StorageKey::parse(name)) {
            [[maybe_unused]] const auto removed =
                ulw::test::send(target, infra::curl::Method::Delete, bucket->object(*key));
        }
    }
}

// Root reads any process's environment, so as root the test runs its targets and the reader
// as nobody instead, without capabilities, like any other user.
constexpr uid_t kNobody = 65534;

// Can another process of the user `pid` runs as, holding no capabilities, read its environment?
bool environ_readable_by_its_user(pid_t pid) {
    const std::string path = "/proc/" + std::to_string(pid) + "/environ";
    const pid_t reader = ::fork();
    if (reader == 0) {
        if (::geteuid() == 0 &&
            (::setgroups(0, nullptr) != 0 || ::setresgid(kNobody, kNobody, kNobody) != 0 ||
             ::setresuid(kNobody, kNobody, kNobody) != 0)) {
            std::_Exit(2);
        }
        const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        char byte = 0;
        std::_Exit(fd >= 0 && ::read(fd, &byte, 1) == 1 ? 0 : 1);
    }
    int status = 0;
    while (::waitpid(reader, &status, 0) < 0 && errno == EINTR) {
    }
    EXPECT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) != 2) << "could not become nobody";
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

template <class Pred> bool within(std::chrono::milliseconds limit, Pred pred) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (!pred()) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        std::this_thread::yield();
    }
    return true;
}

class WorkerTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (ulw::test::run_process({"ffmpeg", "-version"}).exit_code != 0) {
            GTEST_SKIP() << "ffmpeg is not installed";
        }
        // The worker refuses to start without its sandbox (ADR-0025).
        if (const auto refused =
                infra::ffmpeg::check_sandbox(ULW_SANDBOX_BIN, files_.path(), clock_)) {
            GTEST_SKIP() << "this host refuses the sandbox: " << *refused;
        }
        ScratchDatabase::open(db_);
        if (IsSkipped() || HasFatalFailure()) {
            return;
        }
        conn_.emplace(db_->session());
        ASSERT_TRUE(ulw::test::make_clip(
            clip(), {.size = "1280x720", .rate = "30000/1001", .seconds = 8, .audio = true}));
    }

    [[nodiscard]] fs::path clip() const { return files_.path() / "clip.mp4"; }

    // What the gateway's commit leaves: a video in processing and its transcode job queued.
    core::VideoId queue_video(core::ports::IObjectTransfer& store) {
        const auto video = core::VideoId::generate(clock_, random_);
        const std::string source = "videos/" + video.to_string() + "/raw";
        EXPECT_TRUE(store.upload(clip(), *core::StorageKey::parse(source),
                                 *core::ContentType::parse("video/mp4")));
        EXPECT_TRUE(conn_->exec("INSERT INTO videos (id, owner_id, title, state) "
                                "VALUES ($1, 'auth0|tester', 'clip', 'processing')",
                                Params{}.add_uuid(video.uuid())));
        EXPECT_TRUE(conn_->exec("INSERT INTO jobs (video_id, kind, source_key, request_id) "
                                "VALUES ($1, 'transcode', $2, 'req-1')",
                                Params{}.add_uuid(video.uuid()).add_text(source)));
        return video;
    }

    // `wrapper`, when given, starts the worker: its argv comes first.
    std::unique_ptr<ChildProcess> start_worker(const std::string& node,
                                               const std::vector<std::string>& storage_env,
                                               std::vector<std::string> wrapper = {}) {
        auto scratch = std::make_unique<TempDir>("ulw-worker-" + node);
        std::vector<std::string> env{"ULW_DATABASE_URL=" + db_->conninfo(), "ULW_NODE_ID=" + node,
                                     "ULW_SCRATCH_DIR=" + scratch->path().string(),
                                     "PATH=" + env_or("PATH", "/usr/bin:/bin")};
        env.insert(env.end(), storage_env.begin(), storage_env.end());
        if (!wrapper.empty()) {
            // The wrapper may start it as another user.
            fs::permissions(scratch->path(), fs::perms::all);
        }
        scratch_dirs_.push_back(std::move(scratch));
        wrapper.emplace_back(ULW_WORKER_BIN);
        auto worker = ChildProcess::start(wrapper, env);
        EXPECT_NE(worker, nullptr);
        return worker;
    }

    [[nodiscard]] std::vector<std::string> fs_env() const {
        return {"ULW_STORAGE=fs", "ULW_FS_ROOT=" + store_root_.path().string()};
    }

    std::string column(infra::postgres::Sql sql, const core::VideoId& video) {
        return scalar(*conn_, sql, Params{}.add_uuid(video.uuid()));
    }

    // Every video's job as one line, for comparing before and after.
    std::string job_row(const core::VideoId& video) {
        return column("SELECT concat_ws(' ', state, attempts, fence, locked_by, last_error) "
                      "FROM jobs WHERE video_id = $1",
                      video);
    }

    // What the reaper does once a lease lapses, without waiting 60 s for it: the lapsed job is
    // queued again and, rather than after its backoff, due at once.
    void expire_and_requeue() {
        ASSERT_TRUE(conn_->exec("UPDATE jobs SET lease_expires = now() - interval '1 second' "
                                "WHERE state = 'running'"));
        infra::postgres::PgJobQueue reaper(db_->conninfo());
        EXPECT_EQ(reaper.reap_expired(), 1U);
        ASSERT_TRUE(conn_->exec("UPDATE jobs SET run_after = now() WHERE state = 'queued'"));
    }

    // The published tree, fetched back through `store` and checked the way a player would
    // need it: every file each playlist names exists, and keyframes line up across rungs.
    static void expect_published_hls(core::ports::IObjectTransfer& store,
                                     const core::VideoId& video) {
        const TempDir fetched("ulw-fetched");
        const std::string prefix = "videos/" + video.to_string() + "/hls/";
        const auto get = [&](const std::string& rel) {
            const fs::path local = fetched.path() / rel;
            fs::create_directories(local.parent_path());
            return store.download(*core::StorageKey::parse(prefix + rel), local).has_value();
        };
        ASSERT_TRUE(get("master.m3u8"));
        const std::string master = read_text(fetched.path() / "master.m3u8");
        EXPECT_NE(master.find("#EXT-X-VERSION:7"), std::string::npos);
        std::vector<std::string> keyframes;
        for (const std::string rung : {"720p", "360p"}) {
            ASSERT_NE(master.find(rung + "/index.m3u8"), std::string::npos) << master;
            ASSERT_TRUE(get(rung + "/index.m3u8"));
            const auto files = referenced_files(read_text(fetched.path() / rung / "index.m3u8"));
            EXPECT_GE(files.size(), 3U);
            for (const std::string& f : files) {
                EXPECT_TRUE(get(std::format("{}/{}", rung, f))) << rung << "/" << f;
            }
            keyframes.push_back(ulw::test::keyframe_times(fetched.path() / rung / "index.m3u8"));
        }
        EXPECT_EQ(std::ranges::count(keyframes[0], '\n'), 2) << keyframes[0];
        EXPECT_EQ(keyframes[0], keyframes[1]);
    }

    void expect_ready(const core::VideoId& video) {
        EXPECT_EQ(column("SELECT state FROM videos WHERE id = $1", video), "ready");
        const auto duration =
            std::stoi(column("SELECT duration_ms FROM videos WHERE id = $1", video));
        EXPECT_NEAR(duration, 8000, 100);
        EXPECT_EQ(column("SELECT string_agg(height || ':' || playlist_key, ' ' ORDER BY height) "
                         "FROM renditions WHERE video_id = $1",
                         video),
                  "360:videos/" + video.to_string() + "/hls/360p/index.m3u8 720:videos/" +
                      video.to_string() + "/hls/720p/index.m3u8");
    }

    // One video through a worker configured with `storage_env`, against `target` read back
    // directly.
    void run_against(const ulw::test::LiveS3& target, const std::vector<std::string>& storage_env) {
        auto store = infra::storage::S3Transfer::create({.credentials = target.credentials,
                                                         .clock = clock_,
                                                         .random = random_,
                                                         .profile = target.profile,
                                                         .bucket = target.bucket});
        ASSERT_TRUE(store);
        const auto video = queue_video(**store);
        const auto worker = start_worker("worker-a", storage_env);
        ASSERT_TRUE(worker->wait_for_output("outcome=", kJobPatience)) << worker->output();
        EXPECT_NE(worker->output().find("outcome=done"), std::string::npos) << worker->output();
        expect_ready(video);
        expect_published_hls(**store, video);
        worker->signal(SIGTERM);
        EXPECT_EQ(worker->wait_exit(kExitPatience), 0);
        remove_prefix(target, "videos/" + video.to_string() + "/");
    }

    os::SystemClock clock_;
    os::SystemRandom random_;
    std::unique_ptr<ScratchDatabase> db_;
    std::optional<infra::postgres::SyncConnection> conn_;
    TempDir files_{"ulw-worker-files"};
    TempDir store_root_{"ulw-worker-store"};
    infra::storage::FsTransfer fs_store_{store_root_.path()};
    std::vector<std::unique_ptr<TempDir>> scratch_dirs_;
};

TEST_F(WorkerTest, AnUploadedMp4BecomesHlsAndTheVideoReady) {
    const auto video = queue_video(fs_store_);
    const auto worker = start_worker("worker-a", fs_env());
    ASSERT_TRUE(worker->wait_for_output("outcome=", kJobPatience)) << worker->output();
    EXPECT_NE(worker->output().find("outcome=done"), std::string::npos) << worker->output();
    expect_ready(video);
    EXPECT_EQ(job_row(video), "done 1 1 worker-a");
    expect_published_hls(fs_store_, video);
    // Metrics for the job, and a workspace that is gone.
    EXPECT_NE(worker->output().find("realtime="), std::string::npos);
    EXPECT_NE(worker->output().find("ffmpeg_peak_rss_kib="), std::string::npos);
    EXPECT_TRUE(fs::is_empty(scratch_dirs_.back()->path() / "worker-a"));

    worker->signal(SIGTERM);
    EXPECT_EQ(worker->wait_exit(kExitPatience), 0);
}

TEST_F(WorkerTest, RunsAgainstMinio) {
    const ulw::test::LiveS3 minio = ulw::test::minio_from_env();
    if (!ulw::test::ensure_bucket(minio)) {
#ifdef ULW_CONFORMANCE_LIVE
        FAIL() << "MinIO unreachable";
#else
        GTEST_SKIP() << "MinIO unreachable; start deploy/local/compose.yaml";
#endif
    }
    run_against(minio,
                {"ULW_STORAGE=minio",
                 "ULW_S3_ENDPOINT=" + env_or("ULW_MINIO_ENDPOINT", "http://127.0.0.1:9000"),
                 "ULW_BUCKET=" + minio.bucket,
                 "ULW_S3_ACCESS_KEY_ID=" + env_or("ULW_MINIO_ACCESS_KEY", "ulw-dev"),
                 "ULW_S3_SECRET_ACCESS_KEY=" + env_or("ULW_MINIO_SECRET_KEY", "ulw-dev-secret")});
}

TEST_F(WorkerTest, RunsAgainstR2WhenItsCredentialsAreSet) {
    const auto r2 = ulw::test::r2_from_env();
    if (!r2) {
        GTEST_SKIP() << "no R2 credentials: set ULW_R2_ACCOUNT_ID, ULW_R2_ACCESS_KEY_ID, "
                        "ULW_R2_SECRET_ACCESS_KEY and ULW_R2_BUCKET";
    }
    run_against(*r2, {"ULW_STORAGE=r2", "ULW_R2_ACCOUNT_ID=" + env_or("ULW_R2_ACCOUNT_ID", ""),
                      "ULW_BUCKET=" + r2->bucket,
                      "ULW_S3_ACCESS_KEY_ID=" + env_or("ULW_R2_ACCESS_KEY_ID", ""),
                      "ULW_S3_SECRET_ACCESS_KEY=" + env_or("ULW_R2_SECRET_ACCESS_KEY", "")});
}

TEST_F(WorkerTest, ASigkilledWorkersJobIsRetriedByAnotherAndItsFfmpegDiesWithIt) {
    const auto video = queue_video(fs_store_);
    auto a = start_worker("worker-a", fs_env());
    const std::string a_scratch = scratch_dirs_.back()->path().string();
    // Killed mid-transcode, with ffmpeg running.
    ASSERT_TRUE(a->wait_for_output("probed", kJobPatience)) << a->output();
    ASSERT_TRUE(within(seconds(30), [&] { return process_mentions(a_scratch); }));
    a->signal(SIGKILL);
    EXPECT_EQ(a->wait_exit(kExitPatience), 128 + SIGKILL);
    EXPECT_TRUE(within(seconds(10), [&] { return !process_mentions(a_scratch); }))
        << "a child of the killed worker is still running";
    EXPECT_EQ(column("SELECT state FROM jobs WHERE video_id = $1", video), "running");

    expire_and_requeue();
    const auto b = start_worker("worker-b", fs_env());
    ASSERT_TRUE(b->wait_for_output("outcome=", kJobPatience)) << b->output();
    EXPECT_NE(b->output().find("outcome=done"), std::string::npos) << b->output();
    expect_ready(video);
    EXPECT_EQ(job_row(video), "done 2 2 worker-b worker lease expired");
    expect_published_hls(fs_store_, video);
    b->signal(SIGTERM);
    EXPECT_EQ(b->wait_exit(kExitPatience), 0);
}

TEST_F(WorkerTest, AStoppedWorkerThatResumesIsFencedOutAndChangesNothing) {
    const auto video = queue_video(fs_store_);
    auto a = start_worker("worker-a", fs_env());
    ASSERT_TRUE(a->wait_for_output("claimed", kJobPatience)) << a->output();
    a->signal(SIGSTOP);
    // Frozen while it held the job.
    ASSERT_EQ(column("SELECT concat_ws(' ', state, fence, locked_by) FROM jobs WHERE video_id = $1",
                     video),
              "running 1 worker-a");

    expire_and_requeue();
    const auto b = start_worker("worker-b", fs_env());
    ASSERT_TRUE(b->wait_for_output("outcome=", kJobPatience)) << b->output();
    EXPECT_NE(b->output().find("outcome=done"), std::string::npos) << b->output();
    expect_ready(video);
    const std::string job_after_b = job_row(video);
    const std::string video_after_b =
        column("SELECT concat_ws(' ', state, version, duration_ms, updated_at) FROM videos "
               "WHERE id = $1",
               video);
    EXPECT_EQ(job_after_b, "done 2 2 worker-b worker lease expired");

    // The zombie wakes, tries to write under fence 1, and every write matches no row.
    a->signal(SIGCONT);
    ASSERT_TRUE(a->wait_for_output("matched no row", kJobPatience)) << a->output();
    ASSERT_TRUE(a->wait_for_output("outcome=", kJobPatience)) << a->output();
    EXPECT_EQ(a->output().find("outcome=done"), std::string::npos) << a->output();
    EXPECT_EQ(job_row(video), job_after_b);
    EXPECT_EQ(column("SELECT concat_ws(' ', state, version, duration_ms, updated_at) FROM videos "
                     "WHERE id = $1",
                     video),
              video_after_b);
    EXPECT_EQ(column("SELECT count(*) FROM renditions WHERE video_id = $1", video), "2");
    expect_published_hls(fs_store_, video);

    a->signal(SIGTERM);
    EXPECT_EQ(a->wait_exit(kExitPatience), 0);
    b->signal(SIGTERM);
    EXPECT_EQ(b->wait_exit(kExitPatience), 0);
}

TEST_F(WorkerTest, SigtermMidTranscodeGivesTheJobBackAndExitsCleanly) {
    const auto video = queue_video(fs_store_);
    auto a = start_worker("worker-a", fs_env());
    ASSERT_TRUE(a->wait_for_output("probed", kJobPatience)) << a->output();
    a->signal(SIGTERM);
    EXPECT_EQ(a->wait_exit(kExitPatience), 0) << a->output();
    EXPECT_NE(a->output().find("outcome=requeued"), std::string::npos) << a->output();
    EXPECT_EQ(job_row(video), "queued 1 1 worker stopped");
    EXPECT_EQ(column("SELECT state FROM videos WHERE id = $1", video), "processing");
    EXPECT_TRUE(fs::is_empty(scratch_dirs_.back()->path() / "worker-a"));
}

TEST_F(WorkerTest, ItsEnvironmentIsUnreadableToOtherProcessesOfItsUser) {
    // It holds the database password and the storage keys.
    std::vector<std::string> as_user;
    if (::geteuid() == 0) {
        as_user = {"/usr/bin/setpriv", "--reuid=" + std::to_string(kNobody),
                   "--regid=" + std::to_string(kNobody), "--clear-groups", "--"};
    }
    const auto worker = start_worker("worker-a", fs_env(), as_user);
    ASSERT_TRUE(worker->wait_for_output("sandbox=", kExitPatience)) << worker->output();
    // An ordinary process of the same user is readable, so the refusal is the worker's doing.
    as_user.insert(as_user.end(), {"/bin/sleep", "300"});
    const auto ordinary = ChildProcess::start(as_user, {"ULW_DATABASE_URL=x"});
    ASSERT_NE(ordinary, nullptr);
    EXPECT_TRUE(within(seconds(10), [&] { return environ_readable_by_its_user(ordinary->pid()); }));
    EXPECT_FALSE(environ_readable_by_its_user(worker->pid()));
    worker->signal(SIGTERM);
    EXPECT_EQ(worker->wait_exit(kExitPatience), 0);
}

TEST_F(WorkerTest, AnUndecodableUploadFailsTheVideoWithAReason) {
    const auto video = core::VideoId::generate(clock_, random_);
    std::ofstream(clip()) << "not a video at all";
    const std::string source = "videos/" + video.to_string() + "/raw";
    ASSERT_TRUE(fs_store_.upload(clip(), *core::StorageKey::parse(source),
                                 *core::ContentType::parse("video/mp4")));
    ASSERT_TRUE(conn_->exec("INSERT INTO videos (id, owner_id, title, state) "
                            "VALUES ($1, 'auth0|tester', 'clip', 'processing')",
                            Params{}.add_uuid(video.uuid())));
    ASSERT_TRUE(conn_->exec("INSERT INTO jobs (video_id, kind, source_key) "
                            "VALUES ($1, 'transcode', $2)",
                            Params{}.add_uuid(video.uuid()).add_text(source)));
    const auto worker = start_worker("worker-a", fs_env());
    ASSERT_TRUE(worker->wait_for_output("outcome=", kJobPatience)) << worker->output();
    EXPECT_EQ(column("SELECT concat_ws(' ', state, error_reason) FROM videos WHERE id = $1", video),
              "failed the file could not be decoded as video");
    EXPECT_EQ(column("SELECT state FROM jobs WHERE video_id = $1", video), "failed");
    worker->signal(SIGTERM);
    EXPECT_EQ(worker->wait_exit(kExitPatience), 0);
}

} // namespace
