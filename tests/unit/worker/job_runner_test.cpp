#include "core/models/ids.hpp"
#include "os/system_clock.hpp"

#include "fakes.hpp"
#include "job_runner.hpp"
#include "support/fake_random.hpp"
#include "support/temp_dir.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <gtest/gtest.h>
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
                                  .random = random},
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

    [[nodiscard]] bool scratch_is_empty() const {
        return std::filesystem::is_empty(scratch.path());
    }

    Journal journal;
    FakeQueue queue{journal, "queue"};
    FakeQueue lease_queue{journal, "lease"};
    FakeTransfer transfer{journal};
    FakeTranscoder transcoder;
    os::SystemClock clock;
    ulw::test::FakeRandom random;
    ulw::test::TempDir scratch{"ulw-worker-test"};
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
    transcoder.verify_failure = failure(TranscodeFailure::Unverified, 0);
    EXPECT_EQ(run(), JobOutcome::Failed);
    EXPECT_EQ(writes(), std::vector<std::string>{
                            "queue fail permanent the transcoded output failed verification"});
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
         .random = random},
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
