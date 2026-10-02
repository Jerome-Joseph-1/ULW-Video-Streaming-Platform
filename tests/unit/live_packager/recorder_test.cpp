#include "media_playlist.hpp"
#include "recorder.hpp"
#include "support.hpp"
#include "support/fake_clock.hpp"
#include "support/fake_random.hpp"
#include "support/temp_dir.hpp"

#include <algorithm>
#include <array>
#include <functional>
#include <gtest/gtest.h>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

namespace fs = std::filesystem;
using infra::ffmpeg::AudioFormat;
using infra::ffmpeg::RecordingInput;
using infra::ffmpeg::RecordingRemuxJob;
using infra::ffmpeg::RemuxError;
using infra::ffmpeg::RemuxFailure;
using infra::postgres::NewRecording;
using infra::postgres::RecordingRow;
using infra::postgres::RecordingStoreError;
using live::RecordOutcome;

class FakeCatalog final : public live::IRecordingCatalog {
public:
    std::expected<std::optional<RecordingRow>, RecordingStoreError>
    find(std::string_view stream) override {
        if (find_unavailable) {
            return std::unexpected(RecordingStoreError::Unavailable);
        }
        const auto it = rows.find(std::string(stream));
        return it == rows.end() ? std::nullopt : std::optional(it->second);
    }
    std::expected<RecordingRow, RecordingStoreError>
    record(const NewRecording& recording) override {
        if (before_record) {
            before_record();
        }
        if (unavailable) {
            return std::unexpected(RecordingStoreError::Unavailable);
        }
        recorded.push_back(recording);
        if (lose_commit) {
            rows.try_emplace(recording.stream,
                             RecordingRow{.video = recording.video, .failure = {}});
            find_unavailable = find_fails_after_lost_commit;
            return std::unexpected(RecordingStoreError::Unknown);
        }
        if (unknown_outcome) {
            if (winner) {
                rows.try_emplace(recording.stream, RecordingRow{.video = winner, .failure = {}});
            }
            return std::unexpected(RecordingStoreError::Unknown);
        }
        return rows
            .try_emplace(recording.stream, RecordingRow{.video = recording.video, .failure = {}})
            .first->second;
    }
    std::expected<RecordingRow, RecordingStoreError> fail(std::string_view stream,
                                                          std::string_view reason) override {
        return rows
            .try_emplace(std::string(stream),
                         RecordingRow{.video = std::nullopt, .failure = std::string(reason)})
            .first->second;
    }

    std::map<std::string, RecordingRow> rows;
    std::vector<NewRecording> recorded;
    std::function<void()> before_record;
    bool unavailable = false;
    // The row goes in, and the answer is lost on the way back.
    bool lose_commit = false;
    bool find_fails_after_lost_commit = false;
    bool find_unavailable = false;
    // The commit's answer is lost and the row is `winner`'s, or none yet (a commit still on
    // its way).
    bool unknown_outcome = false;
    std::optional<core::VideoId> winner;
};

// The filesystem store, whose streams take nothing: every write answers `error`.
class RefusingStreams final : public core::ports::IObjectStreams {
public:
    explicit RefusingStreams(core::ports::StorageError error) : error_(error) {}

    std::expected<std::unique_ptr<core::ports::IObjectStream>, core::ports::StorageError>
    begin(const core::StorageKey& /*key*/, const core::ContentType& /*type*/,
          std::uint64_t /*max_bytes*/) override {
        return std::make_unique<Refusing>(error_);
    }
    std::expected<void, core::ports::StorageError>
    remove(const core::StorageKey& /*key*/) override {
        return {};
    }

private:
    class Refusing final : public core::ports::IObjectStream {
    public:
        explicit Refusing(core::ports::StorageError error) : error_(error) {}
        std::expected<void, core::ports::StorageError>
        write(std::span<const std::byte> /*bytes*/) override {
            return std::unexpected(error_);
        }
        std::expected<void, core::ports::StorageError> commit() override {
            return std::unexpected(error_);
        }

    private:
        core::ports::StorageError error_;
    };

    core::ports::StorageError error_;
};

// Each stage copies its input to its output unchanged, so the stored recording is exactly the
// pieces fed in, in order.
class PassThroughCopier final : public live::IRecordingCopier {
public:
    std::expected<void, RemuxError> run(const RecordingRemuxJob& job,
                                        const std::function<void(std::string_view)>& on_output,
                                        const std::stop_token& stop) override {
        {
            const std::lock_guard lock(mutex_);
            jobs.push_back(job);
            if (on_run) {
                on_run(job);
            }
        }
        std::array<char, 4096> buffer{};
        while (true) {
            const ssize_t n = ::read(job.input, buffer.data(), buffer.size());
            if (n <= 0) {
                break;
            }
            if (!refuse || (refuse_only && *refuse_only != job.from)) {
                on_output(std::string_view(buffer.data(), static_cast<std::size_t>(n)));
            }
        }
        // As the sandbox does: a stop kills the child, whatever it had read.
        if (stop.stop_requested() && !refuse) {
            const std::lock_guard lock(mutex_);
            stopped_joining = stopped_joining || job.from == RecordingInput::MpegTs;
            return std::unexpected(RemuxError{.kind = RemuxFailure::Stopped, .detail = "stopped"});
        }
        if (refuse && (!refuse_only || *refuse_only == job.from)) {
            return std::unexpected(RemuxError{.kind = *refuse, .detail = "refused"});
        }
        if (refuse) {
            // The other stage, stopped by the one that failed.
            return std::unexpected(RemuxError{.kind = RemuxFailure::Stopped, .detail = "stopped"});
        }
        return {};
    }
    std::expected<std::optional<AudioFormat>, RemuxError>
    probe_audio(const fs::path& init, const fs::path& /*work_dir*/,
                const std::stop_token& /*stop*/) override {
        const auto it = audio.find(init.filename().string());
        return it == audio.end() ? std::optional(AudioFormat{.sample_rate = 48'000, .channels = 2})
                                 : it->second;
    }

    std::vector<RecordingRemuxJob> jobs;
    std::function<void(const RecordingRemuxJob&)> on_run;
    std::map<std::string, std::optional<AudioFormat>> audio;
    std::optional<RemuxFailure> refuse;
    // Only the stage reading this input fails; the other reports being stopped.
    std::optional<RecordingInput> refuse_only;
    bool stopped_joining = false;

private:
    std::mutex mutex_;
};

class RecorderTest : public ::testing::Test {
protected:
    [[nodiscard]] fs::path live_dir() const { return root.path() / "objects/live/show"; }
    [[nodiscard]] fs::path videos_dir() const { return root.path() / "objects/videos"; }

    // Segments first..last of `epoch`, and its init segment, as a run published them.
    void run_of(std::uint32_t epoch, std::uint64_t first, std::uint64_t last) const {
        fs::create_directories(live_dir());
        ulw::test::write_file(live_dir() / ("init_" + std::to_string(epoch) + ".mp4"),
                              "I" + std::to_string(epoch) + ";");
        for (std::uint64_t n = first; n <= last; ++n) {
            ulw::test::write_file(live_dir() / seg(epoch, n), "S" + std::to_string(n) + ";");
        }
    }
    static std::string seg(std::uint32_t epoch, std::uint64_t n) {
        return "seg_" + std::to_string(epoch) + "_" + std::to_string(n) + ".m4s";
    }

    // The stored playlist: the last `count` of segments up to `last`, each of the epoch
    // `epoch_of` gives it.
    void playlist(std::uint64_t last, std::uint64_t count,
                  const std::function<std::uint32_t(std::uint64_t)>& epoch_of,
                  bool ended = true) const {
        live::MediaPlaylist p{.target_seconds = 2,
                              .media_sequence = last + 1 - count,
                              .discontinuity_sequence = 0,
                              .ended = ended,
                              .segments = {}};
        for (std::uint64_t n = p.media_sequence; n <= last; ++n) {
            p.segments.push_back({.uri = seg(epoch_of(n), n),
                                  .duration = live::Micros{2'000'000},
                                  .init = "init_" + std::to_string(epoch_of(n)) + ".mp4",
                                  .discontinuity = false,
                                  .program_date_time = std::nullopt});
        }
        ulw::test::write_file(live_dir() / "index.m3u8", live::render_media_playlist(p));
    }

    live::RecordResult record(std::optional<std::uint32_t> own_claim = std::nullopt,
                              core::ports::IObjectStreams* through = nullptr,
                              std::uint64_t max_bytes = 1U << 20U) {
        const live::RecorderSettings settings{.stream = *live::StreamId::parse("show"),
                                              .owner = *core::UserId::parse("auth0|streamer"),
                                              .work_dir = root.path() / "work",
                                              .wall = core::Seconds{60},
                                              .max_bytes = max_bytes,
                                              .own_claim = own_claim};
        return live::record_stream({.store = store,
                                    .streams = through != nullptr ? *through : streams,
                                    .copier = copier,
                                    .catalog = catalog,
                                    .clock = clock,
                                    .random = random},
                                   settings, {});
    }

    [[nodiscard]] std::string raw_of(const core::VideoId& video) const {
        return ulw::test::read_file(videos_dir() / video.to_string() / "raw");
    }
    [[nodiscard]] std::size_t stored_videos() const {
        std::error_code ec;
        if (!fs::exists(videos_dir(), ec)) {
            return 0;
        }
        return static_cast<std::size_t>(
            std::distance(fs::directory_iterator(videos_dir()), fs::directory_iterator()));
    }

    ulw::test::TempDir root{"ulw-recorder"};
    ulw::test::RecordingStore store{root.path()};
    infra::storage::FsTransfer streams{root.path()};
    PassThroughCopier copier;
    FakeCatalog catalog;
    ulw::test::FakeClock clock;
    ulw::test::FakeRandom random;
};

TEST_F(RecorderTest, AnEndedStreamGoesUnderItsVideosSourceKeyAndIsQueuedOnce) {
    run_of(0, 0, 3);
    playlist(3, 2, [](std::uint64_t) { return 0U; });
    const auto done = record();
    ASSERT_EQ(done.outcome, RecordOutcome::Recorded) << done.detail;
    ASSERT_TRUE(done.video);
    EXPECT_EQ(raw_of(*done.video), "I0;S0;S1;S2;S3;");
    ASSERT_EQ(catalog.recorded.size(), 1U);
    EXPECT_EQ(catalog.recorded[0].video, *done.video);
    EXPECT_EQ(catalog.recorded[0].source.str(), "videos/" + done.video->to_string() + "/raw");
    EXPECT_EQ(catalog.recorded[0].owner.view(), "auth0|streamer");
    // Every child writes into an empty directory of its own, apart from the downloaded pieces.
    ASSERT_EQ(copier.jobs.size(), 2U);
    for (const RecordingRemuxJob& job : copier.jobs) {
        EXPECT_EQ(job.work_dir, root.path() / "work/child");
        EXPECT_TRUE(fs::is_empty(job.work_dir));
    }

    const auto again = record();
    EXPECT_EQ(again.outcome, RecordOutcome::AlreadyRecorded);
    EXPECT_EQ(again.video, done.video);
    EXPECT_EQ(copier.jobs.size(), 2U);
    EXPECT_EQ(stored_videos(), 1U);
}

TEST_F(RecorderTest, EveryRunOfARestartedStreamIsCopiedInOrderBehindItsOwnInitSegment) {
    run_of(0, 0, 2);
    run_of(1, 3, 5);
    playlist(5, 3, [](std::uint64_t) { return 1U; });
    const auto done = record();
    ASSERT_EQ(done.outcome, RecordOutcome::Recorded) << done.detail;
    EXPECT_EQ(raw_of(*done.video), "I0;S0;S1;S2;I1;S3;S4;S5;");
    ASSERT_EQ(copier.jobs.size(), 3U);
    EXPECT_EQ(std::ranges::count(copier.jobs, RecordingInput::MpegTs, &RecordingRemuxJob::from), 1);
}

TEST_F(RecorderTest, ARunWithoutAudioGetsSilenceOfTheFormatAnotherRunCarries) {
    run_of(0, 0, 1);
    run_of(1, 2, 3);
    playlist(3, 2, [](std::uint64_t) { return 1U; });
    copier.audio["init_0.mp4"] = std::nullopt;
    copier.audio["init_1.mp4"] = AudioFormat{.sample_rate = 44'100, .channels = 1};
    ASSERT_EQ(record().outcome, RecordOutcome::Recorded);
    std::map<std::string, std::optional<AudioFormat>> silence;
    for (const RecordingRemuxJob& job : copier.jobs) {
        if (job.from == RecordingInput::FragmentedMp4) {
            silence[job.silence ? "with" : "without"] = job.silence;
        }
    }
    ASSERT_EQ(silence.size(), 2U);
    EXPECT_EQ(silence["with"], (AudioFormat{.sample_rate = 44'100, .channels = 1}));
}

TEST_F(RecorderTest, AStreamThatHasNotEndedHasNothingToRecord) {
    run_of(0, 0, 3);
    playlist(3, 2, [](std::uint64_t) { return 0U; }, /*ended=*/false);
    EXPECT_EQ(record().outcome, RecordOutcome::NothingToRecord);
    EXPECT_TRUE(copier.jobs.empty());
    EXPECT_TRUE(catalog.rows.empty());
}

TEST_F(RecorderTest, AnEndBehindANewerClaimIsAStaleWritersAndNothingIsRecorded) {
    run_of(0, 0, 3);
    playlist(3, 2, [](std::uint64_t) { return 0U; });
    ulw::test::write_file(live_dir() / "epoch_1", "claimed\n");
    const auto done = record();
    EXPECT_EQ(done.outcome, RecordOutcome::Superseded);
    EXPECT_TRUE(copier.jobs.empty());
    EXPECT_TRUE(catalog.rows.empty());
}

TEST_F(RecorderTest, ThisProcesssOwnClaimDoesNotFenceItOut) {
    run_of(0, 0, 3);
    playlist(3, 2, [](std::uint64_t) { return 0U; });
    ulw::test::write_file(live_dir() / "epoch_1", "claimed\n");
    EXPECT_EQ(record(1).outcome, RecordOutcome::Recorded);
}

TEST_F(RecorderTest, APlaylistThatMovesOnDuringTheCopyLeavesNoVideoAndNoObject) {
    run_of(0, 0, 3);
    playlist(3, 2, [](std::uint64_t) { return 0U; });
    // The stale end is overwritten by the newer packager, which goes on publishing.
    copier.on_run = [this](const RecordingRemuxJob&) {
        run_of(1, 4, 4);
        playlist(4, 2, [](std::uint64_t n) { return n < 4 ? 0U : 1U; }, /*ended=*/false);
    };
    const auto done = record();
    EXPECT_EQ(done.outcome, RecordOutcome::Superseded) << done.detail;
    EXPECT_TRUE(catalog.rows.empty());
    EXPECT_EQ(stored_videos(), 1U) << "the video directory remains";
    EXPECT_TRUE(fs::is_empty(fs::directory_iterator(videos_dir())->path()));
}

TEST_F(RecorderTest, ARecorderThatLosesTheRaceForTheRowRemovesItsRecording) {
    run_of(0, 0, 3);
    playlist(3, 2, [](std::uint64_t) { return 0U; });
    const auto winner = core::VideoId::generate(clock, random);
    catalog.before_record = [&] {
        catalog.rows.try_emplace("show", RecordingRow{.video = winner, .failure = {}});
    };
    const auto done = record();
    EXPECT_EQ(done.outcome, RecordOutcome::AlreadyRecorded);
    EXPECT_EQ(done.video, winner);
    ASSERT_EQ(catalog.recorded.size(), 1U);
    EXPECT_FALSE(fs::exists(videos_dir() / catalog.recorded[0].video.to_string() / "raw"));
}

TEST_F(RecorderTest, ADatabaseDownAtTheInsertRemovesTheRecordingForTheNextRunToRedo) {
    run_of(0, 0, 3);
    playlist(3, 2, [](std::uint64_t) { return 0U; });
    catalog.unavailable = true;
    EXPECT_EQ(record().outcome, RecordOutcome::Failed);
    EXPECT_TRUE(catalog.rows.empty());
    for (const auto& dir : fs::directory_iterator(videos_dir())) {
        EXPECT_TRUE(fs::is_empty(dir.path()));
    }
    catalog.unavailable = false;
    EXPECT_EQ(record().outcome, RecordOutcome::Recorded);
}

TEST_F(RecorderTest, WhatARunKilledAfterItsCommitLeftIsNeverReferenced) {
    run_of(0, 0, 3);
    playlist(3, 2, [](std::uint64_t) { return 0U; });
    const auto orphan = core::VideoId::generate(clock, random);
    fs::create_directories(videos_dir() / orphan.to_string());
    ulw::test::write_file(videos_dir() / orphan.to_string() / "raw", "a killed run's");
    const auto done = record();
    ASSERT_EQ(done.outcome, RecordOutcome::Recorded);
    EXPECT_NE(done.video, orphan);
    EXPECT_EQ(raw_of(*done.video), "I0;S0;S1;S2;S3;");
    EXPECT_EQ(raw_of(orphan), "a killed run's");
}

TEST_F(RecorderTest, AMissingInitSegmentMarksTheStreamUnrecordableOnce) {
    run_of(0, 0, 3);
    playlist(3, 2, [](std::uint64_t) { return 0U; });
    fs::remove(live_dir() / "init_0.mp4");
    const auto done = record();
    EXPECT_EQ(done.outcome, RecordOutcome::Unrecordable);
    ASSERT_TRUE(catalog.rows.contains("show"));
    EXPECT_EQ(catalog.rows["show"].failure, "init_0.mp4 is missing");
    EXPECT_EQ(record().outcome, RecordOutcome::AlreadyRecorded);
    EXPECT_TRUE(copier.jobs.empty());
}

TEST_F(RecorderTest, InputFfmpegRefusesMarksTheStreamUnrecordableAndStoresNothing) {
    run_of(0, 0, 3);
    playlist(3, 2, [](std::uint64_t) { return 0U; });
    copier.refuse = RemuxFailure::Refused;
    EXPECT_EQ(record().outcome, RecordOutcome::Unrecordable);
    EXPECT_FALSE(catalog.rows["show"].failure.empty());
    for (const auto& dir : fs::directory_iterator(videos_dir())) {
        EXPECT_TRUE(fs::is_empty(dir.path()));
    }
}

TEST_F(RecorderTest, ASandboxThatCannotStartIsLeftForTheNextRun) {
    run_of(0, 0, 3);
    playlist(3, 2, [](std::uint64_t) { return 0U; });
    copier.refuse = RemuxFailure::Unavailable;
    EXPECT_EQ(record().outcome, RecordOutcome::Failed);
    EXPECT_TRUE(catalog.rows.empty());
}

TEST_F(RecorderTest, ARunThatEndedTheStreamWithoutASegmentOfItsOwnIsRecorded) {
    // Epoch 1 claimed and died; epoch 2 is this run, which ended the stream without media.
    run_of(0, 0, 3);
    playlist(3, 2, [](std::uint64_t) { return 0U; });
    ulw::test::write_file(live_dir() / "epoch_1", "claimed\n");
    ulw::test::write_file(live_dir() / "epoch_2", "claimed\n");
    EXPECT_EQ(record(2).outcome, RecordOutcome::Recorded);
}

TEST_F(RecorderTest, AClaimAboveThisRunsOwnStillFencesItOut) {
    run_of(0, 0, 3);
    playlist(3, 2, [](std::uint64_t) { return 0U; });
    ulw::test::write_file(live_dir() / "epoch_2", "claimed\n");
    ulw::test::write_file(live_dir() / "epoch_3", "claimed\n");
    EXPECT_EQ(record(2).outcome, RecordOutcome::Superseded);
    EXPECT_TRUE(catalog.rows.empty());
}

TEST_F(RecorderTest, AnInsertThatCommittedButWasReportedFailedKeepsItsRecording) {
    run_of(0, 0, 3);
    playlist(3, 2, [](std::uint64_t) { return 0U; });
    catalog.lose_commit = true;
    const auto done = record();
    ASSERT_EQ(done.outcome, RecordOutcome::Recorded) << done.detail;
    EXPECT_EQ(raw_of(*done.video), "I0;S0;S1;S2;S3;");
}

TEST_F(RecorderTest, ARecordingIsKeptWhenNothingCanSayWhetherItsRowWentIn) {
    run_of(0, 0, 3);
    playlist(3, 2, [](std::uint64_t) { return 0U; });
    catalog.lose_commit = true;
    catalog.find_fails_after_lost_commit = true;
    EXPECT_EQ(record().outcome, RecordOutcome::Failed);
    ASSERT_EQ(catalog.recorded.size(), 1U);
    EXPECT_EQ(raw_of(catalog.recorded[0].video), "I0;S0;S1;S2;S3;");
}

TEST_F(RecorderTest, TheJoiningStageRefusingMarksTheStreamUnrecordableRatherThanStopped) {
    run_of(0, 0, 3);
    playlist(3, 2, [](std::uint64_t) { return 0U; });
    copier.refuse = RemuxFailure::Refused;
    copier.refuse_only = RecordingInput::MpegTs;
    const auto done = record();
    EXPECT_EQ(done.outcome, RecordOutcome::Unrecordable) << done.detail;
    EXPECT_NE(catalog.rows["show"].failure.find("joining the runs"), std::string::npos);
}

TEST_F(RecorderTest, AStoreThatRefusesTheCredentialsLeavesTheStreamForTheNextRun) {
    run_of(0, 0, 3);
    playlist(3, 2, [](std::uint64_t) { return 0U; });
    for (const auto error :
         {core::ports::StorageError::Unauthorized, core::ports::StorageError::Permanent}) {
        RefusingStreams refusing(error);
        EXPECT_EQ(record(std::nullopt, &refusing).outcome, RecordOutcome::Failed);
        EXPECT_TRUE(catalog.rows.empty());
    }
}

TEST_F(RecorderTest, ARecordingPastItsBoundMarksTheStreamUnrecordable) {
    run_of(0, 0, 3);
    playlist(3, 2, [](std::uint64_t) { return 0U; });
    const auto done = record(std::nullopt, nullptr, 10);
    EXPECT_EQ(done.outcome, RecordOutcome::Unrecordable) << done.detail;
    EXPECT_NE(catalog.rows["show"].failure.find("past its bound"), std::string::npos);
}

TEST_F(RecorderTest, TheEndersWrittenEpochKeepsARestartFromTakingItsClaimForANewerRun) {
    // Epoch 1 ended the stream without a segment of its own, then failed to record it; the
    // restart holds no claim, and only ended_by says epoch_1 was the ender's.
    run_of(0, 0, 3);
    playlist(3, 2, [](std::uint64_t) { return 0U; });
    ulw::test::write_file(live_dir() / "epoch_1", "claimed\n");
    ulw::test::write_file(live_dir() / "ended_by", "1\n");
    EXPECT_EQ(record().outcome, RecordOutcome::Recorded);
}

TEST_F(RecorderTest, AClaimAboveTheEnderStillFencesARestartOut) {
    run_of(0, 0, 3);
    playlist(3, 2, [](std::uint64_t) { return 0U; });
    ulw::test::write_file(live_dir() / "epoch_1", "claimed\n");
    ulw::test::write_file(live_dir() / "epoch_2", "claimed\n");
    ulw::test::write_file(live_dir() / "ended_by", "1\n");
    EXPECT_EQ(record().outcome, RecordOutcome::Superseded);
}

TEST_F(RecorderTest, AnEmptyOrMalformedEndedByIsIgnoredNotFatal) {
    run_of(0, 0, 3);
    playlist(3, 2, [](std::uint64_t) { return 0U; });
    ulw::test::write_file(live_dir() / "epoch_1", "claimed\n");
    for (const std::string& bad : {std::string(), std::string("one\n"), std::string(64, '9')}) {
        ulw::test::write_file(live_dir() / "ended_by", bad);
        // The fence falls back to the last segment's run: epoch 1 is then a newer claim.
        EXPECT_EQ(record().outcome, RecordOutcome::Superseded) << bad;
        EXPECT_TRUE(catalog.rows.empty());
    }
    // And with this process's own claim it records.
    EXPECT_EQ(record(1).outcome, RecordOutcome::Recorded);
}

TEST_F(RecorderTest, AnInsertOfUnknownOutcomeWithNoRowYetKeepsItsRecordingAndFails) {
    run_of(0, 0, 3);
    playlist(3, 2, [](std::uint64_t) { return 0U; });
    catalog.unknown_outcome = true;
    EXPECT_EQ(record().outcome, RecordOutcome::Failed);
    ASSERT_EQ(catalog.recorded.size(), 1U);
    EXPECT_EQ(raw_of(catalog.recorded[0].video), "I0;S0;S1;S2;S3;");
}

TEST_F(RecorderTest, AnInsertOfUnknownOutcomeBeatenByAnotherVideoRemovesItsRecording) {
    run_of(0, 0, 3);
    playlist(3, 2, [](std::uint64_t) { return 0U; });
    const auto other = core::VideoId::generate(clock, random);
    catalog.unknown_outcome = true;
    catalog.winner = other;
    const auto done = record();
    EXPECT_EQ(done.outcome, RecordOutcome::AlreadyRecorded);
    EXPECT_EQ(done.video, other);
    ASSERT_EQ(catalog.recorded.size(), 1U);
    EXPECT_FALSE(fs::exists(videos_dir() / catalog.recorded[0].video.to_string() / "raw"));
}

TEST_F(RecorderTest, AFailedInsertBeatenByAnotherVideoRemovesItsRecording) {
    run_of(0, 0, 3);
    playlist(3, 2, [](std::uint64_t) { return 0U; });
    const auto other = core::VideoId::generate(clock, random);
    catalog.before_record = [&] {
        catalog.rows.try_emplace("show", RecordingRow{.video = other, .failure = {}});
    };
    catalog.unavailable = true;
    const auto done = record();
    EXPECT_EQ(done.outcome, RecordOutcome::AlreadyRecorded);
    EXPECT_EQ(done.video, other);
    for (const auto& dir : fs::directory_iterator(videos_dir())) {
        EXPECT_TRUE(fs::is_empty(dir.path())) << dir.path();
    }
}

TEST_F(RecorderTest, AFeedThatFailsStopsTheJoiningStageRatherThanEndingItsInput) {
    run_of(0, 0, 3);
    playlist(3, 2, [](std::uint64_t) { return 0U; });
    // A segment the plan found that is unreadable by the time it is copied: a store error.
    store.download_error_for = "seg_0_2.m4s";
    EXPECT_EQ(record().outcome, RecordOutcome::Failed);
    for (const RecordingRemuxJob& job : copier.jobs) {
        if (job.from == RecordingInput::MpegTs) {
            EXPECT_TRUE(copier.stopped_joining) << "the joining stage saw a clean end of input";
        }
    }
    EXPECT_TRUE(catalog.rows.empty());
}

TEST_F(RecorderTest, AStoredPlaylistTheStoreCannotReadLeavesTheStreamForTheNextRun) {
    run_of(0, 0, 3);
    playlist(3, 2, [](std::uint64_t) { return 0U; });
    store.download_error_for = "index.m3u8";
    const auto done = record();
    EXPECT_EQ(done.outcome, RecordOutcome::Failed) << done.detail;
    EXPECT_NE(done.detail.find("playlist unreadable"), std::string::npos) << done.detail;
    EXPECT_TRUE(copier.jobs.empty());
    EXPECT_TRUE(catalog.rows.empty());
    EXPECT_EQ(stored_videos(), 0U);

    // Once the store answers, the same stream is recorded.
    store.download_error_for.clear();
    EXPECT_EQ(record().outcome, RecordOutcome::Recorded);
}

// Past its bound or unparsable, a stored playlist is not one this packager wrote; nothing is
// copied, and with no end known, nothing is marked either.
TEST_F(RecorderTest, AStoredPlaylistPastItsBoundOrUnparsableIsUnrecordableAndMarksNothing) {
    run_of(0, 0, 3);
    ulw::test::write_file(live_dir() / "index.m3u8", std::string((1U << 20U) + 1, '#'));
    const auto large = record();
    EXPECT_EQ(large.outcome, RecordOutcome::Unrecordable) << large.detail;
    EXPECT_NE(large.detail.find("too large"), std::string::npos) << large.detail;

    ulw::test::write_file(live_dir() / "index.m3u8", "#EXTM3U\n#EXT-X-TARGETDURATION:x\n");
    const auto invalid = record();
    EXPECT_EQ(invalid.outcome, RecordOutcome::Unrecordable) << invalid.detail;
    EXPECT_NE(invalid.detail.find("invalid"), std::string::npos) << invalid.detail;

    EXPECT_TRUE(copier.jobs.empty());
    EXPECT_TRUE(catalog.rows.empty());
    EXPECT_EQ(stored_videos(), 0U);
}

TEST(RecordingBound, TheLongestStreamAtTheHighestBitrateWithAnEighthForTheContainer) {
    // 100 Mbit/s for 12 hours is 540 GB.
    EXPECT_EQ(live::recording_bound(100'000, core::Seconds{12 * 3600}), 607'500'000'000U);
}

} // namespace
