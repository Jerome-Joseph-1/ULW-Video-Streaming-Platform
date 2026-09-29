// A live stream's recording, end to end: the ffmpeg test publisher over SRT into live_packager,
// the stream ended, and the one video that becomes taken to ready by transcode_worker, all
// separate processes on a scratch Postgres database. The M33 acceptance runs, including the
// stream whose end is seen twice, the packager that dies mid-stream, and the workers that die
// or come back from the dead while transcoding a recording.
#include "core/models/ids.hpp"
#include "infra/ffmpeg/transcoder.hpp"
#include "infra/postgres/job_queue.hpp"
#include "infra/storage/fs_transfer.hpp"
#include "infra/storage/s3_transfer.hpp"
#include "os/system_clock.hpp"
#include "os/system_random.hpp"
#include "os/unique_fd.hpp"

#include "postgres_harness.hpp"
#include "support/child_process.hpp"
#include "support/live_s3.hpp"
#include "support/temp_dir.hpp"

#include <sys/syscall.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <gtest/gtest.h>
#include <memory>
#include <optional>
#include <poll.h>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

namespace fs = std::filesystem;
using infra::postgres::Params;
using std::chrono::milliseconds;
using std::chrono::seconds;
using ulw::test::ChildProcess;
using ulw::test::scalar;
using ulw::test::ScratchDatabase;
using ulw::test::TempDir;

// A recording is remuxed twice and transcoded once; a loaded runner does that slowly.
constexpr auto kJobPatience = seconds(180);
constexpr auto kExitPatience = seconds(60);
constexpr auto kSamplePeriod = milliseconds(250);
constexpr std::string_view kPassphrase = "an integration passphrase";
constexpr std::string_view kOwner = "auth0|streamer";

std::string env_or(const char* name, const std::string& fallback) {
    // Read while no thread exists that could setenv.
    // NOLINTNEXTLINE(concurrency-mt-unsafe)
    const char* value = std::getenv(name);
    return value == nullptr || *value == '\0' ? fallback : value;
}

std::string read_text(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::string text;
    std::array<char, 4096> buffer{};
    while (in.read(buffer.data(), buffer.size()) || in.gcount() > 0) {
        text.append(buffer.data(), static_cast<std::size_t>(in.gcount()));
    }
    return text;
}

// The lines of a playlist that name files.
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

// Segments the stored live playlist has published so far: its media sequence plus its length.
std::uint64_t published_segments(const std::string& playlist) {
    constexpr std::string_view kSequence = "#EXT-X-MEDIA-SEQUENCE:";
    const std::size_t at = playlist.find(kSequence);
    if (at == std::string::npos) {
        return 0;
    }
    const std::uint64_t first = std::stoull(playlist.substr(at + kSequence.size()));
    std::uint64_t listed = 0;
    for (std::size_t p = playlist.find("#EXTINF"); p != std::string::npos;
         p = playlist.find("#EXTINF", p + 1)) {
        ++listed;
    }
    return first + listed;
}

// The processes whose command line mentions `needle`; the worker's ffmpeg children all name
// their workspace.
std::vector<pid_t> processes_mentioning(const std::string& needle) {
    std::vector<pid_t> found;
    for (const auto& entry : fs::directory_iterator("/proc")) {
        const std::string pid = entry.path().filename().string();
        if (!std::ranges::all_of(pid, [](char c) { return c >= '0' && c <= '9'; })) {
            continue;
        }
        std::string cmdline = read_text(entry.path() / "cmdline");
        std::ranges::replace(cmdline, '\0', ' ');
        // A zombie still lists its command line until it is reaped.
        const bool zombie = read_text(entry.path() / "stat").find(") Z ") != std::string::npos;
        if (!zombie && cmdline.find(needle) != std::string::npos) {
            found.push_back(static_cast<pid_t>(std::stol(pid)));
        }
    }
    return found;
}

// Waits on each process's pidfd, woken by its exit rather than by polling; true once all have
// exited within `limit`. A pid already gone counts as exited.
bool all_exit(const std::vector<pid_t>& pids, milliseconds limit) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    for (const pid_t pid : pids) {
        const os::UniqueFd fd(static_cast<int>(::syscall(SYS_pidfd_open, pid, 0)));
        if (!fd) {
            continue;
        }
        const auto left =
            std::chrono::duration_cast<milliseconds>(deadline - std::chrono::steady_clock::now());
        pollfd exited{.fd = fd.get(), .events = POLLIN, .revents = 0};
        if (left.count() <= 0 || ::poll(&exited, 1, static_cast<int>(left.count())) != 1) {
            return false;
        }
    }
    return true;
}

class LiveRecordingTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (ulw::test::run_process({"ffmpeg", "-version"}).exit_code != 0) {
            GTEST_SKIP() << "ffmpeg is not installed";
        }
        if (const auto refused =
                infra::ffmpeg::check_sandbox(ULW_SANDBOX_BIN, files_.path(), clock_)) {
            GTEST_SKIP() << "this host refuses the sandbox: " << *refused;
        }
        ScratchDatabase::open(db_);
        if (IsSkipped() || HasFatalFailure()) {
            return;
        }
        conn_.emplace(db_->session());
        stream_ = "rec-" + ulw::test::unique_prefix("s").substr(2, 16);
    }

    void TearDown() override {
        if (minio_) {
            ulw::test::remove_objects(*minio_, "live/" + stream_ + "/");
            for (const std::string& video : videos()) {
                ulw::test::remove_objects(*minio_, "videos/" + video + "/");
            }
        }
    }

    // Both programs on MinIO instead of the shared filesystem root.
    void use_minio() {
        minio_ = ulw::test::minio_from_env();
        if (!ulw::test::ensure_bucket(*minio_)) {
#ifdef ULW_CONFORMANCE_LIVE
            FAIL() << "MinIO unreachable";
#else
            GTEST_SKIP() << "MinIO unreachable; start deploy/local/compose.yaml";
#endif
        }
        auto store = infra::storage::S3Transfer::create({.credentials = minio_->credentials,
                                                         .clock = clock_,
                                                         .random = random_,
                                                         .profile = minio_->profile,
                                                         .bucket = minio_->bucket});
        ASSERT_TRUE(store);
        s3_ = std::move(*store);
    }

    core::ports::IObjectTransfer& store() {
        return s3_ ? static_cast<core::ports::IObjectTransfer&>(*s3_) : fs_store_;
    }

    [[nodiscard]] std::vector<std::string> storage_env() const {
        if (!minio_) {
            return {"ULW_STORAGE=fs", "ULW_FS_ROOT=" + store_root_.path().string()};
        }
        return {"ULW_STORAGE=minio",
                "ULW_S3_ENDPOINT=" + env_or("ULW_MINIO_ENDPOINT", "http://127.0.0.1:9000"),
                "ULW_BUCKET=" + minio_->bucket,
                "ULW_S3_ACCESS_KEY_ID=" + env_or("ULW_MINIO_ACCESS_KEY", "ulw-dev"),
                "ULW_S3_SECRET_ACCESS_KEY=" + env_or("ULW_MINIO_SECRET_KEY", "ulw-dev-secret")};
    }

    // `recording` false: a packager that ends the stream but has no database, as one killed
    // between the end and the job would leave it.
    std::unique_ptr<ChildProcess> start_packager(bool recording = true) {
        scratch_dirs_.push_back(std::make_unique<TempDir>("ulw-rec-scratch"));
        std::vector<std::string> env{"PATH=" + env_or("PATH", "/usr/bin:/bin"),
                                     "ULW_STREAM_ID=" + stream_,
                                     "ULW_LIVE_INGEST_PORT=0",
                                     "ULW_LIVE_SRT_PASSPHRASE=" + std::string(kPassphrase),
                                     "ULW_LIVE_WINDOW_SEGMENTS=3",
                                     "ULW_LIVE_SEGMENT_SECONDS=2",
                                     "ULW_SCRATCH_DIR=" + scratch_dirs_.back()->path().string(),
                                     std::string("ULW_SANDBOX_BIN=") + ULW_SANDBOX_BIN};
        const auto storage = storage_env();
        env.insert(env.end(), storage.begin(), storage.end());
        if (recording) {
            env.push_back("ULW_DATABASE_URL=" + db_->conninfo());
            env.push_back("ULW_STREAM_OWNER=" + std::string(kOwner));
        }
        auto packager = ChildProcess::start({ULW_LIVE_PACKAGER_BIN}, env);
        EXPECT_NE(packager, nullptr);
        return packager;
    }

    static std::optional<std::uint16_t> ingest_port(ChildProcess& packager) {
        constexpr std::string_view kMark = "ingest=127.0.0.1:";
        if (!packager.wait_for_output(kMark, seconds(20))) {
            return std::nullopt;
        }
        const std::string& out = packager.output();
        const std::size_t at = out.find(kMark) + kMark.size();
        return static_cast<std::uint16_t>(std::stoul(out.substr(at, out.find(' ', at) - at)));
    }

    // 720p, so the recording is transcoded to two rungs whose keyframes must line up. A
    // duration of 0 means until killed.
    [[nodiscard]] std::unique_ptr<ChildProcess>
    start_publisher(std::uint16_t port, unsigned duration, bool audio = true) const {
        std::vector<std::string> argv{ULW_LIVE_TESTSOURCE, "127.0.0.1:" + std::to_string(port)};
        if (duration != 0) {
            argv.push_back(std::to_string(duration));
        }
        auto publisher =
            ChildProcess::start(argv, {"PATH=" + env_or("PATH", "/usr/bin:/bin"),
                                       "ULW_TESTSOURCE_PASSPHRASE=" + std::string(kPassphrase),
                                       "ULW_TESTSOURCE_STREAMID=" + stream_,
                                       "ULW_TESTSOURCE_SIZE=1280x720", "ULW_TESTSOURCE_KBPS=2500",
                                       std::string("ULW_TESTSOURCE_AUDIO=") + (audio ? "1" : "0")});
        EXPECT_NE(publisher, nullptr);
        return publisher;
    }

    // A packager run from the first byte to the end of a publisher that sends `duration`
    // seconds and hangs up; returns its exit code.
    std::optional<int> stream_for(unsigned duration, bool recording = true) {
        const auto packager = start_packager(recording);
        const auto port = ingest_port(*packager);
        EXPECT_TRUE(port) << packager->output();
        if (!port) {
            return std::nullopt;
        }
        const auto publisher = start_publisher(*port, duration);
        EXPECT_EQ(publisher->wait_exit(kExitPatience), 0) << publisher->output();
        const auto code = packager->wait_exit(kJobPatience);
        last_output_ = packager->output();
        return code;
    }

    std::optional<std::string> stored_text(const std::string& key) {
        const TempDir dir("ulw-rec-fetch");
        if (!store().download(*core::StorageKey::parse(key), dir.path() / "f")) {
            return std::nullopt;
        }
        return read_text(dir.path() / "f");
    }

    std::vector<std::string> videos() {
        if (!conn_) {
            return {};
        }
        const std::string all =
            scalar(*conn_, "SELECT coalesce(string_agg(id::text, ' ' ORDER BY id), '') FROM videos",
                   Params{});
        std::vector<std::string> ids;
        for (std::size_t at = 0; at < all.size();) {
            const std::size_t end = std::min(all.find(' ', at), all.size());
            ids.push_back(all.substr(at, end - at));
            at = end + 1;
        }
        return ids;
    }

    std::string query(infra::postgres::Sql sql) { return scalar(*conn_, sql, Params{}); }

    std::unique_ptr<ChildProcess> start_worker(const std::string& node) {
        auto scratch = std::make_unique<TempDir>("ulw-rec-worker-" + node);
        std::vector<std::string> env{"ULW_DATABASE_URL=" + db_->conninfo(), "ULW_NODE_ID=" + node,
                                     "ULW_SCRATCH_DIR=" + scratch->path().string(),
                                     "PATH=" + env_or("PATH", "/usr/bin:/bin"), "ULW_ALLOW_ROOT=1"};
        const auto storage = storage_env();
        env.insert(env.end(), storage.begin(), storage.end());
        scratch_dirs_.push_back(std::move(scratch));
        auto worker = ChildProcess::start({ULW_WORKER_BIN}, env);
        EXPECT_NE(worker, nullptr);
        return worker;
    }

    // What the reaper does once a lease lapses, without waiting 60 s for it.
    void expire_and_requeue() {
        ASSERT_TRUE(conn_->exec("UPDATE jobs SET lease_expires = now() - interval '1 second' "
                                "WHERE state = 'running'"));
        infra::postgres::PgJobQueue reaper(db_->conninfo());
        EXPECT_EQ(reaper.reap_expired(), 1U);
        ASSERT_TRUE(conn_->exec("UPDATE jobs SET run_after = now() WHERE state = 'queued'"));
    }

    // The single video of the stream, and the one job it came with.
    [[nodiscard]] std::string the_video() {
        const auto ids = videos();
        EXPECT_EQ(ids.size(), 1U);
        return ids.empty() ? std::string() : ids.front();
    }

    std::string job_row() {
        return query("SELECT concat_ws(' ', state, attempts, fence, locked_by, last_error) "
                     "FROM jobs");
    }

    // Exactly one video, ready, owned by the streamer, of about `seconds_long`, with a master
    // playlist whose every rendition, and every file those name, is in the store.
    void expect_one_ready_video(double seconds_long) {
        const std::string video = the_video();
        EXPECT_EQ(query("SELECT count(*) FROM jobs"), "1");
        EXPECT_EQ(scalar(*conn_, "SELECT concat_ws(' ', state, owner_id) FROM videos", Params{}),
                  "ready " + std::string(kOwner));
        const double duration = std::stod(query("SELECT duration_ms FROM videos")) / 1000.0;
        EXPECT_NEAR(duration, seconds_long, 2.5);
        EXPECT_EQ(query("SELECT string_agg(height::text, ' ' ORDER BY height) FROM renditions"),
                  "360 720");

        const std::string prefix = "videos/" + video + "/hls/";
        const auto master = stored_text(prefix + "master.m3u8");
        ASSERT_TRUE(master);
        for (const std::string& rung : referenced_files(*master)) {
            const auto media = stored_text(prefix + rung);
            ASSERT_TRUE(media) << rung;
            const auto files = referenced_files(*media);
            EXPECT_GE(files.size(), 3U) << *media;
            const std::string dir = rung.substr(0, rung.rfind('/') + 1);
            for (const std::string& f : files) {
                const std::string key = std::format("{}{}{}", prefix, dir, f);
                EXPECT_TRUE(store().size(*core::StorageKey::parse(key))) << key;
            }
        }
    }

    void run_worker_to_done() {
        const auto worker = start_worker("worker-a");
        ASSERT_TRUE(worker->wait_for_output(R"("outcome":")", kJobPatience)) << worker->output();
        EXPECT_NE(worker->output().find(R"("outcome":"done")"), std::string::npos)
            << worker->output();
        worker->signal(SIGTERM);
        EXPECT_EQ(worker->wait_exit(kExitPatience), 0);
    }

    os::SystemClock clock_;
    os::SystemRandom random_;
    std::unique_ptr<ScratchDatabase> db_;
    std::optional<infra::postgres::SyncConnection> conn_;
    TempDir files_{"ulw-rec-files"};
    TempDir store_root_{"ulw-rec-store"};
    infra::storage::FsTransfer fs_store_{store_root_.path()};
    std::optional<ulw::test::LiveS3> minio_;
    std::unique_ptr<infra::storage::S3Transfer> s3_;
    std::string stream_;
    std::string last_output_;
    std::vector<std::unique_ptr<TempDir>> scratch_dirs_;
};

TEST_F(LiveRecordingTest, AnEndedStreamBecomesExactlyOneVideoThatGoesFromProcessingToReady) {
    use_minio();
    if (IsSkipped() || HasFatalFailure()) {
        return;
    }
    ASSERT_EQ(stream_for(10), 0) << last_output_;
    EXPECT_NE(last_output_.find("recording: queued as video"), std::string::npos) << last_output_;
    const std::string video = the_video();
    EXPECT_EQ(query("SELECT state FROM videos"), "processing");
    EXPECT_EQ(query("SELECT concat_ws(' ', state, source_key, request_id) FROM jobs"),
              "queued videos/" + video + "/raw " + stream_);
    EXPECT_EQ(query("SELECT video_id FROM live_recordings"), video);

    run_worker_to_done();
    expect_one_ready_video(10);
}

TEST_F(LiveRecordingTest, AStreamWhoseEndIsSeenAgainIsStillOneVideo) {
    // Ended by a packager that died before it could queue anything.
    ASSERT_EQ(stream_for(8, /*recording=*/false), 0) << last_output_;
    EXPECT_EQ(query("SELECT count(*) FROM videos"), "0");

    // The next packager for the stream finds it ended and records it; the ones after find it
    // recorded.
    for (int run = 0; run < 3; ++run) {
        const auto packager = start_packager();
        ASSERT_EQ(packager->wait_exit(kJobPatience), 0) << packager->output();
        EXPECT_NE(packager->output().find(run == 0 ? "recording: queued as video"
                                                   : "recording: already video"),
                  std::string::npos)
            << packager->output();
        EXPECT_EQ(packager->output().find("ingest="), std::string::npos)
            << "an ended stream took a publisher";
    }
    EXPECT_EQ(query("SELECT count(*) FROM videos"), "1");
    EXPECT_EQ(query("SELECT count(*) FROM jobs"), "1");

    run_worker_to_done();
    const auto again = start_packager();
    ASSERT_EQ(again->wait_exit(kJobPatience), 0) << again->output();
    expect_one_ready_video(8);
}

TEST_F(LiveRecordingTest, AStreamWhosePackagerDiedMidwayIsRecordedAsOneVideoOfBothRuns) {
    std::uint64_t first_run = 0;
    {
        const auto packager = start_packager();
        const auto port = ingest_port(*packager);
        ASSERT_TRUE(port) << packager->output();
        // The first run carries no audio and the second does: the recording still has one
        // audio stream end to end, silent where the first run was.
        const auto publisher = start_publisher(*port, 0, /*audio=*/false);
        const std::string playlist = "live/" + stream_ + "/index.m3u8";
        while (published_segments(stored_text(playlist).value_or("")) < 4) {
            ASSERT_FALSE(publisher->wait_exit(kSamplePeriod)) << publisher->output();
        }
        packager->signal(SIGKILL);
        EXPECT_EQ(packager->wait_exit(kExitPatience), 128 + SIGKILL);
        publisher->signal(SIGKILL);
        first_run = published_segments(stored_text(playlist).value_or(""));
    }
    EXPECT_EQ(query("SELECT count(*) FROM videos"), "0");

    ASSERT_EQ(stream_for(8), 0) << last_output_;
    EXPECT_NE(last_output_.find("in 2 runs, 0 missing"), std::string::npos) << last_output_;
    run_worker_to_done();
    // Every segment either run published, end to end: the second run's timestamps began again
    // at zero, and the recording carries the timeline on across that.
    expect_one_ready_video((static_cast<double>(first_run) * 2.0) + 8.0);
    const auto master = stored_text("videos/" + the_video() + "/hls/master.m3u8");
    ASSERT_TRUE(master);
    EXPECT_NE(master->find("mp4a"), std::string::npos) << *master;
}

TEST_F(LiveRecordingTest, AStreamEndedByARestartedPackagerWithNoMediaOfItsOwnIsRecorded) {
    std::uint64_t published = 0;
    {
        const auto packager = start_packager();
        const auto port = ingest_port(*packager);
        ASSERT_TRUE(port) << packager->output();
        const auto publisher = start_publisher(*port, 0);
        const std::string playlist = "live/" + stream_ + "/index.m3u8";
        while (published_segments(stored_text(playlist).value_or("")) < 3) {
            ASSERT_FALSE(publisher->wait_exit(kSamplePeriod)) << publisher->output();
        }
        packager->signal(SIGKILL);
        EXPECT_EQ(packager->wait_exit(kExitPatience), 128 + SIGKILL);
        publisher->signal(SIGKILL);
        published = published_segments(stored_text(playlist).value_or(""));
    }
    // The restart claims epoch 1, and is told to end the stream before any publisher comes:
    // it ends the first run's window, and must not take its own claim for a newer packager's.
    const auto packager = start_packager();
    ASSERT_TRUE(ingest_port(*packager)) << packager->output();
    packager->signal(SIGUSR1);
    ASSERT_EQ(packager->wait_exit(kJobPatience), 0) << packager->output();
    EXPECT_NE(packager->output().find("recording: queued as video"), std::string::npos)
        << packager->output();
    run_worker_to_done();
    expect_one_ready_video(static_cast<double>(published) * 2.0);
}

TEST_F(LiveRecordingTest, TwoPackagersRecordingOneEndedStreamAtOnceMakeOneVideoAndOneSource) {
    ASSERT_EQ(stream_for(8, /*recording=*/false), 0) << last_output_;
    auto a = start_packager();
    auto b = start_packager();
    ASSERT_EQ(a->wait_exit(kJobPatience), 0) << a->output();
    ASSERT_EQ(b->wait_exit(kJobPatience), 0) << b->output();
    const std::string outputs = a->output() + b->output();
    EXPECT_NE(outputs.find("recording: queued as video"), std::string::npos) << outputs;
    EXPECT_NE(outputs.find("recording: already video"), std::string::npos) << outputs;
    EXPECT_EQ(query("SELECT count(*) FROM videos"), "1");
    EXPECT_EQ(query("SELECT count(*) FROM jobs"), "1");
    // The loser removed the recording it made; only the video's own source is left.
    std::size_t sources = 0;
    for (const auto& dir : fs::directory_iterator(store_root_.path() / "objects/videos")) {
        sources += fs::exists(dir.path() / "raw") ? 1U : 0U;
    }
    EXPECT_EQ(sources, 1U);
    EXPECT_TRUE(fs::exists(store_root_.path() / "objects/videos" / the_video() / "raw"));
}

TEST_F(LiveRecordingTest, ALiveSourcedJobSurvivesASigkilledWorkerAndItsFfmpegDiesWithIt) {
    ASSERT_EQ(stream_for(10), 0) << last_output_;
    auto a = start_worker("worker-a");
    const std::string a_scratch = scratch_dirs_.back()->path().string();
    ASSERT_TRUE(a->wait_for_output(R"("event":"probed")", kJobPatience)) << a->output();
    std::vector<pid_t> children = processes_mentioning(a_scratch);
    for (auto ticks = seconds(30) / kSamplePeriod; children.empty() && ticks > 0; --ticks) {
        // The worker's own wait is the tick: it returns at its exit, or after the period.
        ASSERT_FALSE(a->wait_exit(kSamplePeriod)) << a->output();
        children = processes_mentioning(a_scratch);
    }
    ASSERT_FALSE(children.empty()) << "the worker's ffmpeg never started";
    a->signal(SIGKILL);
    EXPECT_EQ(a->wait_exit(kExitPatience), 128 + SIGKILL);
    EXPECT_TRUE(all_exit(processes_mentioning(a_scratch), seconds(10)))
        << "a child of the killed worker is still running";
    EXPECT_EQ(query("SELECT state FROM jobs"), "running");

    expire_and_requeue();
    const auto b = start_worker("worker-b");
    ASSERT_TRUE(b->wait_for_output(R"("outcome":")", kJobPatience)) << b->output();
    EXPECT_NE(b->output().find(R"("outcome":"done")"), std::string::npos) << b->output();
    EXPECT_EQ(job_row(), "done 2 2 worker-b worker lease expired");
    expect_one_ready_video(10);
    b->signal(SIGTERM);
    EXPECT_EQ(b->wait_exit(kExitPatience), 0);
}

TEST_F(LiveRecordingTest, AStaleWorkerOnALiveSourcedJobIsFencedOutAndPublishesNothing) {
    ASSERT_EQ(stream_for(10), 0) << last_output_;
    auto a = start_worker("worker-a");
    ASSERT_TRUE(a->wait_for_output(R"("event":"job claimed")", kJobPatience)) << a->output();
    a->signal(SIGSTOP);
    ASSERT_EQ(query("SELECT concat_ws(' ', state, fence, locked_by) FROM jobs"),
              "running 1 worker-a");

    expire_and_requeue();
    const auto b = start_worker("worker-b");
    ASSERT_TRUE(b->wait_for_output(R"("outcome":")", kJobPatience)) << b->output();
    EXPECT_NE(b->output().find(R"("outcome":"done")"), std::string::npos) << b->output();
    const std::string job_after_b = job_row();
    const std::string video_after_b =
        query("SELECT concat_ws(' ', state, version, duration_ms, updated_at) FROM videos");
    const std::string renditions_after_b =
        query("SELECT string_agg(concat_ws(':', height, bitrate_bps, playlist_key), ' ' ORDER BY "
              "height) FROM renditions");
    const std::string master_after_b =
        stored_text("videos/" + the_video() + "/hls/master.m3u8").value_or("");
    EXPECT_EQ(job_after_b, "done 2 2 worker-b worker lease expired");

    // The zombie wakes, finds its lease gone, and every write it attempts matches no row.
    a->signal(SIGCONT);
    ASSERT_TRUE(a->wait_for_output(R"("event":"fenced out")", kJobPatience)) << a->output();
    ASSERT_TRUE(a->wait_for_output(R"("outcome":")", kJobPatience)) << a->output();
    EXPECT_EQ(a->output().find(R"("outcome":"done")"), std::string::npos) << a->output();
    EXPECT_EQ(job_row(), job_after_b);
    EXPECT_EQ(query("SELECT concat_ws(' ', state, version, duration_ms, updated_at) FROM videos"),
              video_after_b);
    EXPECT_EQ(query("SELECT string_agg(concat_ws(':', height, bitrate_bps, playlist_key), ' ' "
                    "ORDER BY height) FROM renditions"),
              renditions_after_b);
    EXPECT_EQ(query("SELECT count(*) FROM renditions"), "2");
    EXPECT_EQ(stored_text("videos/" + the_video() + "/hls/master.m3u8"), master_after_b);
    expect_one_ready_video(10);

    a->signal(SIGTERM);
    EXPECT_EQ(a->wait_exit(kExitPatience), 0);
    b->signal(SIGTERM);
    EXPECT_EQ(b->wait_exit(kExitPatience), 0);
}

} // namespace
