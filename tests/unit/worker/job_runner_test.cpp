#include "core/models/ids.hpp"
#include "core/models/ladder.hpp"
#include "core/util/json.hpp"
#include "os/system_clock.hpp"

#include "fakes.hpp"
#include "job_runner.hpp"
#include "support/fake_random.hpp"
#include "support/memory_log.hpp"
#include "support/temp_dir.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace {

using core::ports::TranscodeError;
using core::ports::TranscodeFailure;
using ulw::test::FakeQueue;
using ulw::test::FakeTranscoder;
using ulw::test::FakeTransfer;
using ulw::test::Journal;
using worker::Disposition;
using worker::JobOutcome;

constexpr std::string_view kVideo = "0192f3c4-7a1b-7c2d-8e3f-0123456789ab";
const std::string kPrefix = "videos/" + std::string(kVideo) + "/hls/";

TranscodeError failure(TranscodeFailure kind, int exit_code = 1) {
    return {.kind = kind, .exit_code = exit_code, .detail = "scripted"};
}

bool is_write(const std::string& event) {
    return !event.ends_with("heartbeat") && !event.starts_with("lease ");
}

class JobRunnerTest : public ::testing::Test {
protected:
    JobRunnerTest() { transfer.put(source_key(), std::string(4096, 'v')); }

    static std::string source_key() { return "videos/" + std::string(kVideo) + "/raw"; }

    JobOutcome run(const std::stop_token& shutdown = {}) {
        const core::ports::ClaimedJob job{.lease = {.job = core::ports::JobId{7}, .fence = 3},
                                          .video = *core::VideoId::parse(kVideo),
                                          .kind = core::ports::JobKind::Transcode,
                                          .source = *core::StorageKey::parse(source_key()),
                                          .request_id = "req-1"};
        worker::JobRunner runner({.queue = queue,
                                  .lease_queue = lease_queue,
                                  .store = transfer,
                                  .transcoder = transcoder,
                                  .clock = clock,
                                  .random = random,
                                  .free_space = free_space(),
                                  .log = log},
                                 {.scratch = scratch.path(),
                                  .node = *core::NodeId::parse("worker-a"),
                                  .lease = intervals});
        return runner.run(job, shutdown);
    }

    [[nodiscard]] std::vector<std::string> writes() const {
        std::vector<std::string> out;
        std::ranges::copy_if(journal.events(), std::back_inserter(out), is_write);
        return out;
    }

    [[nodiscard]] worker::FreeSpace free_space() {
        return [this](const std::filesystem::path&) { return free_bytes; };
    }

    [[nodiscard]] bool scratch_is_empty() const {
        return std::filesystem::is_empty(scratch.path());
    }

    Journal journal;
    FakeQueue queue{journal, "queue"};
    FakeQueue lease_queue{journal, "lease"};
    FakeTransfer transfer{journal};
    FakeTranscoder transcoder;
    os::SystemClock clock;
    ulw::test::MemoryLog lines;
    ops::Logger log{lines, clock, "worker", ops::Level::Debug};
    ulw::test::FakeRandom random;
    ulw::test::TempDir scratch{"ulw-worker-test"};
    // What the scratch filesystem reports free, whenever the runner asks.
    std::optional<std::uint64_t> free_bytes = std::uint64_t{1} << 40U;
    // Long enough that the keeper stays out of the way unless a test shortens it.
    worker::LeaseKeeper::Intervals intervals{.heartbeat = std::chrono::hours(1),
                                             .progress = std::chrono::hours(1)};
};

TEST_F(JobRunnerTest, PublishesSegmentsThenPlaylistsThenTheMasterAndOnlyThenFinishes) {
    EXPECT_EQ(run(), JobOutcome::Done);
    const std::vector<std::string> expected{
        "upload " + kPrefix + "720p/init_0.mp4 video/mp4",
        "upload " + kPrefix + "720p/seg_00000.m4s video/iso.segment",
        "upload " + kPrefix + "720p/seg_00001.m4s video/iso.segment",
        "upload " + kPrefix + "360p/init_1.mp4 video/mp4",
        "upload " + kPrefix + "360p/seg_00000.m4s video/iso.segment",
        "upload " + kPrefix + "360p/seg_00001.m4s video/iso.segment",
        "upload " + kPrefix + "720p/index.m3u8 application/vnd.apple.mpegurl",
        "upload " + kPrefix + "360p/index.m3u8 application/vnd.apple.mpegurl",
        "upload " + kPrefix + "master.m3u8 application/vnd.apple.mpegurl",
        "queue finish 8000 720:2928000:" + kPrefix + "720p/index.m3u8 360:928000:" + kPrefix +
            "360p/index.m3u8"};
    EXPECT_EQ(writes(), expected);
    EXPECT_TRUE(scratch_is_empty());
}

TEST_F(JobRunnerTest, ChecksTheLeaseAfterTranscodingAndPublishesNothingWhenItIsGone) {
    queue.answer_heartbeat(false);
    EXPECT_EQ(run(), JobOutcome::FencedOut);
    EXPECT_TRUE(writes().empty());
    EXPECT_TRUE(scratch_is_empty());
}

TEST_F(JobRunnerTest, AnUnreachableDatabaseBeforePublishingDoesNotStopTheJob) {
    // The finish is fenced anyway; not knowing is no reason to throw the work away.
    queue.answer_heartbeat(std::nullopt);
    EXPECT_EQ(run(), JobOutcome::Done);
}

TEST_F(JobRunnerTest, ACrashIsRunOnceMoreAndCanThenSucceed) {
    transcoder.run_failures.push_back(failure(TranscodeFailure::Crashed, 139));
    EXPECT_EQ(run(), JobOutcome::Done);
    EXPECT_EQ(transcoder.runs, 2);
}

TEST_F(JobRunnerTest, ASecondCrashFailsTheJobForGood) {
    transcoder.run_failures.push_back(failure(TranscodeFailure::Crashed, 139));
    transcoder.run_failures.push_back(failure(TranscodeFailure::Crashed, 139));
    EXPECT_EQ(run(), JobOutcome::Failed);
    EXPECT_EQ(transcoder.runs, 2);
    EXPECT_EQ(writes(),
              std::vector<std::string>{"queue fail permanent the decoder crashed on this file"});
}

TEST_F(JobRunnerTest, ACrashWhileVerifyingIsVerifiedOnceMore) {
    // ffprobe or ffmpeg reading our own output crashed: the same exit code rule applies.
    transcoder.verify_failures.push_back(failure(TranscodeFailure::Crashed, 139));
    EXPECT_EQ(run(), JobOutcome::Done);
    EXPECT_EQ(transcoder.verifies, 2);
    EXPECT_EQ(transcoder.runs, 1);
}

TEST_F(JobRunnerTest, ASecondCrashWhileVerifyingFailsTheJobForGood) {
    transcoder.verify_failures.push_back(failure(TranscodeFailure::Crashed, 139));
    transcoder.verify_failures.push_back(failure(TranscodeFailure::Crashed, 139));
    EXPECT_EQ(run(), JobOutcome::Failed);
    EXPECT_EQ(transcoder.verifies, 2);
    EXPECT_EQ(writes(),
              std::vector<std::string>{"queue fail permanent the decoder crashed on this file"});
}

TEST_F(JobRunnerTest, ASourceTooSmallForAnyRungFailsTheJob) {
    transcoder.media.width = 640;
    transcoder.media.height = 1;
    EXPECT_EQ(run(), JobOutcome::Failed);
    EXPECT_EQ(transcoder.runs, 0);
    EXPECT_EQ(writes(),
              std::vector<std::string>{"queue fail permanent the video is too small to encode"});
}

TEST_F(JobRunnerTest, ARejectedInputFailsTheJobWithoutARerun) {
    transcoder.run_failures.push_back(failure(TranscodeFailure::Rejected));
    EXPECT_EQ(run(), JobOutcome::Failed);
    EXPECT_EQ(transcoder.runs, 1);
    EXPECT_EQ(writes(), std::vector<std::string>{
                            "queue fail permanent the file could not be decoded as video"});
    EXPECT_TRUE(scratch_is_empty());
}

TEST_F(JobRunnerTest, AKilledTranscoderGivesTheJobBack) {
    transcoder.run_failures.push_back(failure(TranscodeFailure::Killed, 137));
    EXPECT_EQ(run(), JobOutcome::Requeued);
    EXPECT_EQ(writes(), std::vector<std::string>{"queue fail retryable the transcoder was killed"});
}

TEST_F(JobRunnerTest, OutputThatFailsVerificationIsNeverPublished) {
    transcoder.verify_failures.push_back(failure(TranscodeFailure::Unverified, 0));
    EXPECT_EQ(run(), JobOutcome::Failed);
    EXPECT_EQ(writes(), std::vector<std::string>{
                            "queue fail permanent the transcoded output failed verification"});
}

TEST_F(JobRunnerTest, AnOutputTheScratchSpaceCannotHoldGivesTheJobBackBeforeTranscoding) {
    // 8 s of 720p and 360p with audio: 3856 kbit/s, 3.86 MB, 4.82 MB with the margin.
    free_bytes = worker::output_bytes(transcoder.media.duration, core::choose_ladder(720), true);
    ASSERT_EQ(*free_bytes, 4'820'000U);
    *free_bytes -= 1;
    EXPECT_EQ(run(), JobOutcome::Requeued);
    EXPECT_EQ(transcoder.runs, 0);
    EXPECT_EQ(writes(),
              std::vector<std::string>{"queue fail retryable no scratch space for the output"});
    EXPECT_TRUE(scratch_is_empty());
}

TEST_F(JobRunnerTest, OutputThatFailsVerificationOnAFullDiskIsRetriedNotFailed) {
    // ffmpeg exits 0 on a full disk and leaves output that cannot pass verification.
    transcoder.during_run = [this](core::ports::ITranscodeProgress&, const std::stop_token&) {
        free_bytes = 4096;
    };
    transcoder.verify_failures.push_back(failure(TranscodeFailure::Unverified, 0));
    EXPECT_EQ(run(), JobOutcome::Requeued);
    EXPECT_EQ(writes(), std::vector<std::string>{"queue fail retryable scratch space ran out"});
}

TEST_F(JobRunnerTest, ATranscodeThatFailsOnAFullDiskIsRetriedNotFailed) {
    transcoder.during_run = [this](core::ports::ITranscodeProgress&, const std::stop_token&) {
        free_bytes = 0;
    };
    transcoder.run_failures.push_back(failure(TranscodeFailure::Rejected, 1));
    EXPECT_EQ(run(), JobOutcome::Requeued);
    EXPECT_EQ(writes(), std::vector<std::string>{"queue fail retryable scratch space ran out"});
}

// Whatever the worker can read and a sandboxed ffmpeg cannot: its environment, say.
class JobRunnerLinkTest : public JobRunnerTest {
protected:
    JobRunnerLinkTest() { std::ofstream(secret_.path() / "environ") << "ULW_DATABASE_URL=secret"; }

    [[nodiscard]] bool secret_uploaded() const {
        return std::ranges::any_of(transfer.objects(), [](const auto& object) {
            return object.second.find("secret") != std::string::npos;
        });
    }

    ulw::test::TempDir secret_{"ulw-worker-secret"};
};

TEST_F(JobRunnerLinkTest, ALinkAmongTheSegmentsIsNeverFollowed) {
    transcoder.after_run = [this](const std::filesystem::path& out) {
        std::filesystem::create_symlink(secret_.path() / "environ", out / "720p" / "seg_00002.m4s");
    };
    EXPECT_EQ(run(), JobOutcome::Requeued);
    EXPECT_FALSE(secret_uploaded());
    EXPECT_EQ(transfer.objects().count(kPrefix + "master.m3u8"), 0U);
}

TEST_F(JobRunnerLinkTest, ALinkedMasterPlaylistIsNeverFollowed) {
    transcoder.after_run = [this](const std::filesystem::path& out) {
        std::filesystem::remove(out / "master.m3u8");
        std::filesystem::create_symlink(secret_.path() / "environ", out / "master.m3u8");
    };
    EXPECT_EQ(run(), JobOutcome::Requeued);
    EXPECT_FALSE(secret_uploaded());
    EXPECT_EQ(transfer.objects().count(kPrefix + "master.m3u8"), 0U);
}

TEST_F(JobRunnerLinkTest, ALinkedRungDirectoryIsNeverFollowed) {
    // Every name in it is one publish expects, and every file in it is a regular file.
    const auto elsewhere = secret_.path() / "rung";
    std::filesystem::create_directory(elsewhere);
    std::filesystem::copy_file(secret_.path() / "environ", elsewhere / "seg_00000.m4s");
    std::ofstream(elsewhere / "index.m3u8") << "#EXTM3U\n";
    transcoder.after_run = [elsewhere](const std::filesystem::path& out) {
        std::filesystem::remove_all(out / "360p");
        std::filesystem::create_directory_symlink(elsewhere, out / "360p");
    };
    EXPECT_EQ(run(), JobOutcome::Requeued);
    EXPECT_FALSE(secret_uploaded());
}

TEST_F(JobRunnerTest, AMissingSourceFailsTheJob) {
    FakeTransfer empty(journal);
    const core::ports::ClaimedJob job{.lease = {.job = core::ports::JobId{7}, .fence = 3},
                                      .video = *core::VideoId::parse(kVideo),
                                      .kind = core::ports::JobKind::Transcode,
                                      .source = *core::StorageKey::parse(source_key()),
                                      .request_id = {}};
    worker::JobRunner runner(
        {.queue = queue,
         .lease_queue = lease_queue,
         .store = empty,
         .transcoder = transcoder,
         .clock = clock,
         .random = random,
         .free_space = free_space(),
         .log = log},
        {.scratch = scratch.path(), .node = *core::NodeId::parse("worker-a"), .lease = intervals});
    EXPECT_EQ(runner.run(job, {}), JobOutcome::Failed);
    EXPECT_EQ(writes(),
              std::vector<std::string>{"queue fail permanent the uploaded file is missing"});
    EXPECT_EQ(transcoder.runs, 0);
}

TEST_F(JobRunnerTest, ALeaseLostMidTranscodeStopsItAndWritesNothingMore) {
    lease_queue.answer_heartbeat(false);
    intervals.heartbeat = std::chrono::milliseconds(1);
    transcoder.during_run = [](core::ports::ITranscodeProgress&, const std::stop_token& stop) {
        // As ffmpeg would, run until told to stop, but never past the test's patience.
        std::mutex m;
        std::condition_variable_any cv;
        std::unique_lock lock(m);
        cv.wait_for(lock, stop, std::chrono::seconds(10), [] { return false; });
    };
    EXPECT_EQ(run(), JobOutcome::Abandoned);
    EXPECT_TRUE(writes().empty());
    EXPECT_TRUE(scratch_is_empty());
}

TEST_F(JobRunnerTest, ALeaseLostWhilePublishingStopsTheUploadsBeforeTheMaster) {
    // The lease is still held at the heartbeat just before publishing, and lost after the
    // first object is written.
    intervals.heartbeat = std::chrono::milliseconds(1);
    std::stop_token abandon;
    transcoder.during_run = [&abandon](core::ports::ITranscodeProgress&,
                                       const std::stop_token& stop) { abandon = stop; };
    bool lost = false;
    transfer.after_upload = [&](const std::string&) {
        if (lost) {
            return;
        }
        lease_queue.answer_heartbeat(false);
        std::mutex m;
        std::condition_variable_any cv;
        std::unique_lock lock(m);
        cv.wait_for(lock, abandon, std::chrono::seconds(10), [] { return false; });
        lost = abandon.stop_requested();
    };
    EXPECT_EQ(run(), JobOutcome::Abandoned);
    ASSERT_TRUE(lost);
    EXPECT_EQ(writes(),
              std::vector<std::string>{"upload " + kPrefix + "720p/init_0.mp4 video/mp4"});
    EXPECT_EQ(transfer.objects().count(kPrefix + "master.m3u8"), 0U);
}

TEST_F(JobRunnerTest, ShutdownMidTranscodeGivesTheJobBackAtOnce) {
    std::stop_source shutdown;
    transcoder.during_run = [&shutdown](core::ports::ITranscodeProgress&, const std::stop_token&) {
        shutdown.request_stop();
    };
    EXPECT_EQ(run(shutdown.get_token()), JobOutcome::Requeued);
    EXPECT_EQ(writes(), std::vector<std::string>{"queue fail retryable worker stopped"});
}

TEST_F(JobRunnerTest, AFinishThatMatchesNoRowIsReportedAsFencedOut) {
    queue.answer_writes(false);
    EXPECT_EQ(run(), JobOutcome::FencedOut);
}

TEST_F(JobRunnerTest, AnUnrecordableFinishLeavesTheJobToItsLease) {
    queue.answer_writes(std::nullopt);
    EXPECT_EQ(run(), JobOutcome::Unrecorded);
}

TEST_F(JobRunnerTest, AResultTheDatabaseRefusesFailsTheJobRatherThanRerunningIt) {
    queue.refuse("finish", core::ports::JobQueueError::Invalid);
    EXPECT_EQ(run(), JobOutcome::Failed);
    const auto w = writes();
    ASSERT_FALSE(w.empty());
    EXPECT_EQ(w.back(), "queue fail permanent the transcoded result could not be recorded");
    const auto errors = lines.events("job queue call failed");
    ASSERT_EQ(errors.size(), 1U);
    EXPECT_NE(errors[0].find(R"("level":"error")"), std::string::npos) << errors[0];
    EXPECT_NE(errors[0].find(R"("call":"finish","error":"invalid")"), std::string::npos);
}

// A refused finish may be a lost lease's: the fail that follows is fenced like every write,
// so a job another worker holds is never failed by this one.
TEST_F(JobRunnerTest, ARefusedFinishFollowedByAFencedFailIsFencedOut) {
    queue.refuse("finish", core::ports::JobQueueError::Invalid);
    queue.answer_writes(false);
    EXPECT_EQ(run(), JobOutcome::FencedOut);
    EXPECT_TRUE(writes().back().starts_with("queue fail permanent")) << writes().back();
}

TEST_F(JobRunnerTest, AnUnreachableDatabaseAtFinishWritesNothingMore) {
    queue.refuse("finish", core::ports::JobQueueError::Unavailable);
    EXPECT_EQ(run(), JobOutcome::Unrecorded);
    EXPECT_FALSE(writes().back().starts_with("queue fail"));
    const auto errors = lines.events("job queue call failed");
    ASSERT_EQ(errors.size(), 1U);
    EXPECT_NE(errors[0].find(R"("level":"warn")"), std::string::npos) << errors[0];
}

TEST_F(JobRunnerTest, ARefusedFailureIsLeftToTheLeaseAndReportedAsAnError) {
    transcoder.run_failures.push_back(failure(TranscodeFailure::Rejected));
    queue.refuse("fail", core::ports::JobQueueError::Invalid);
    EXPECT_EQ(run(), JobOutcome::Unrecorded);
    const auto errors = lines.events("job queue call failed");
    ASSERT_EQ(errors.size(), 1U);
    EXPECT_NE(errors[0].find(R"("call":"fail","error":"invalid")"), std::string::npos);
}

TEST_F(JobRunnerTest, EveryJobEndsWithOneLineOfItsNumbersAndItsRequestId) {
    EXPECT_EQ(run(), JobOutcome::Done);
    const auto finished = lines.events("job finished");
    ASSERT_EQ(finished.size(), 1U);
    const auto doc = core::json::parse(finished[0]);
    ASSERT_TRUE(doc) << finished[0];
    EXPECT_EQ(doc->find("job")->as_u64(), 7U);
    EXPECT_EQ(doc->find("request_id")->as_string(), "req-1");
    EXPECT_EQ(doc->find("outcome")->as_string(), "done");
    EXPECT_EQ(doc->find("svc")->as_string(), "worker");
    for (const char* key : {"wall_ms", "media_ms", "transcode_ms", "realtime", "ffmpeg_exit",
                            "ffmpeg_peak_rss_kib", "worker_peak_rss_kib"}) {
        EXPECT_NE(doc->find(key), nullptr) << key;
    }
}

TEST_F(JobRunnerTest, ProgressReachesTheQueueFromTheKeepersSession) {
    intervals.progress = std::chrono::milliseconds(1);
    bool seen = false;
    transcoder.during_run = [&](core::ports::ITranscodeProgress& progress, const std::stop_token&) {
        progress.on_progress(core::Millis{2000});
        seen = journal.wait_for([](const std::string& e) { return e == "lease progress 25"; },
                                std::chrono::seconds(10));
    };
    EXPECT_EQ(run(), JobOutcome::Done);
    EXPECT_TRUE(seen);
}

TEST(Disposition, FollowsTheExitCodeRules) {
    EXPECT_EQ(worker::disposition(TranscodeFailure::Crashed, false), Disposition::RerunOnce);
    EXPECT_EQ(worker::disposition(TranscodeFailure::Crashed, true), Disposition::FailPermanently);
    EXPECT_EQ(worker::disposition(TranscodeFailure::Killed, false), Disposition::Requeue);
    EXPECT_EQ(worker::disposition(TranscodeFailure::Sandbox, false), Disposition::Requeue);
    EXPECT_EQ(worker::disposition(TranscodeFailure::Rejected, false), Disposition::FailPermanently);
    EXPECT_EQ(worker::disposition(TranscodeFailure::OverBudget, false),
              Disposition::FailPermanently);
    EXPECT_EQ(worker::disposition(TranscodeFailure::Unverified, false),
              Disposition::FailPermanently);
    EXPECT_EQ(worker::disposition(TranscodeFailure::Stopped, false), Disposition::Abandon);
}

} // namespace
