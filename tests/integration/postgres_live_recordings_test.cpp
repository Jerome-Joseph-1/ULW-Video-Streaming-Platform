// PgLiveRecordings against a scratch database: an ended stream becomes one video and one job,
// however many times its end is recorded.
#include "infra/postgres/live_recordings.hpp"
#include "os/system_clock.hpp"
#include "os/system_random.hpp"

#include "postgres_harness.hpp"

#include <gtest/gtest.h>
#include <memory>
#include <optional>
#include <string>
#include <thread>

namespace {

using infra::postgres::NewRecording;
using infra::postgres::Params;
using infra::postgres::PgLiveRecordings;
using infra::postgres::RecordingRow;
using ulw::test::scalar;
using ulw::test::ScratchDatabase;

class LiveRecordingsTest : public ::testing::Test {
protected:
    void SetUp() override {
        ScratchDatabase::open(db_);
        if (IsSkipped() || HasFatalFailure()) {
            return;
        }
        conn_.emplace(db_->session());
    }

    NewRecording recording(const std::string& stream) {
        const auto video = core::VideoId::generate(clock_, random_);
        return {.stream = stream,
                .video = video,
                .owner = *core::UserId::parse("auth0|streamer"),
                .title = "Live stream " + stream,
                .source = *core::StorageKey::parse("videos/" + video.to_string() + "/raw")};
    }

    std::string count(infra::postgres::Sql sql) { return scalar(*conn_, sql, Params{}); }

    os::SystemClock clock_;
    os::SystemRandom random_;
    std::unique_ptr<ScratchDatabase> db_;
    std::optional<infra::postgres::SyncConnection> conn_;
};

TEST_F(LiveRecordingsTest, AStreamBecomesAVideoInProcessingWithAQueuedTranscodeJob) {
    PgLiveRecordings recordings(db_->conninfo());
    EXPECT_EQ(recordings.find("s1"), std::nullopt);
    const NewRecording first = recording("s1");
    const RecordingRow recorded{.video = first.video, .failure = {}};
    ASSERT_EQ(recordings.record(first), recorded);

    EXPECT_EQ(recordings.find("s1"), recorded);
    EXPECT_EQ(scalar(*conn_,
                     "SELECT concat_ws(' ', owner_id, state, title) FROM videos WHERE id = $1",
                     Params{}.add_uuid(first.video.uuid())),
              "auth0|streamer processing Live stream s1");
    EXPECT_EQ(scalar(*conn_,
                     "SELECT concat_ws(' ', kind, state, source_key, request_id) FROM jobs "
                     "WHERE video_id = $1",
                     Params{}.add_uuid(first.video.uuid())),
              "transcode queued videos/" + first.video.to_string() + "/raw s1");
}

TEST_F(LiveRecordingsTest, RecordingAStreamAgainWritesNothingAndNamesTheFirstVideo) {
    PgLiveRecordings recordings(db_->conninfo());
    const NewRecording first = recording("s1");
    ASSERT_EQ(recordings.record(first)->video, first.video);
    // A job that has already finished is no longer live, so the jobs table's own uniqueness
    // would not stop a second one; the stream's row does.
    ASSERT_TRUE(conn_->exec("UPDATE jobs SET state = 'done'"));
    const NewRecording again = recording("s1");
    EXPECT_EQ(recordings.record(again)->video, first.video);

    EXPECT_EQ(count("SELECT count(*) FROM videos"), "1");
    EXPECT_EQ(count("SELECT count(*) FROM jobs"), "1");
    EXPECT_EQ(count("SELECT count(*) FROM live_recordings"), "1");
}

TEST_F(LiveRecordingsTest, TwoStreamsAreTwoVideos) {
    PgLiveRecordings recordings(db_->conninfo());
    ASSERT_TRUE(recordings.record(recording("s1")));
    ASSERT_TRUE(recordings.record(recording("s2")));
    EXPECT_EQ(count("SELECT count(*) FROM videos WHERE state = 'processing'"), "2");
    EXPECT_EQ(count("SELECT count(*) FROM jobs WHERE state = 'queued'"), "2");
}

TEST_F(LiveRecordingsTest, TwoRecordersAtOnceMakeOneVideoAndBothAreToldWhich) {
    const NewRecording a = recording("s1");
    const NewRecording b = recording("s1");
    std::expected<RecordingRow, infra::postgres::RecordingStoreError> got_a;
    std::expected<RecordingRow, infra::postgres::RecordingStoreError> got_b;
    {
        const std::jthread first([&] { got_a = PgLiveRecordings(db_->conninfo()).record(a); });
        const std::jthread second([&] { got_b = PgLiveRecordings(db_->conninfo()).record(b); });
    }
    ASSERT_TRUE(got_a && got_b);
    ASSERT_TRUE(got_a->video);
    EXPECT_EQ(got_a->video, got_b->video);
    EXPECT_TRUE(got_a->video == a.video || got_a->video == b.video);
    EXPECT_EQ(count("SELECT count(*) FROM videos"), "1");
    EXPECT_EQ(count("SELECT count(*) FROM jobs"), "1");
}

TEST_F(LiveRecordingsTest, AStreamThatCannotBeRecordedIsMarkedOnceAndNeverRecordedAfter) {
    PgLiveRecordings recordings(db_->conninfo());
    const RecordingRow failed{.video = std::nullopt, .failure = "segments expired"};
    EXPECT_EQ(recordings.fail("s1", "segments expired"), failed);
    EXPECT_EQ(recordings.fail("s1", "another reason")->failure, "segments expired");
    EXPECT_EQ(recordings.record(recording("s1"))->failure, "segments expired");
    EXPECT_EQ(count("SELECT count(*) FROM videos"), "0");
    EXPECT_EQ(count("SELECT count(*) FROM jobs"), "0");
}

TEST_F(LiveRecordingsTest, ARecordedStreamIsNotMarkedFailedAfterwards) {
    PgLiveRecordings recordings(db_->conninfo());
    const NewRecording first = recording("s1");
    ASSERT_TRUE(recordings.record(first));
    EXPECT_EQ(recordings.fail("s1", "too late")->video, first.video);
}

TEST_F(LiveRecordingsTest, AnEmptyReasonIsRefusedByTheSchema) {
    EXPECT_FALSE(PgLiveRecordings(db_->conninfo()).fail("s1", ""));
    EXPECT_EQ(count("SELECT count(*) FROM live_recordings"), "0");
}

TEST(LiveRecordingsUnreachable, AnUnreachableDatabaseIsUnavailableNotAnAnswer) {
    PgLiveRecordings recordings("postgresql://ulw@127.0.0.1:1/ulw?connect_timeout=2");
    EXPECT_EQ(recordings.find("s1").error(), infra::postgres::RecordingStoreError::Unavailable);
}

} // namespace
