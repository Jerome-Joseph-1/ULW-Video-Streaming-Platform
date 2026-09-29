#include "publisher.hpp"
#include "support.hpp"
#include "support/fake_clock.hpp"
#include "support/temp_dir.hpp"

#include <algorithm>
#include <fstream>
#include <gtest/gtest.h>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
using core::ports::StorageError;
using live::PublishError;
using ulw::test::ffmpeg_playlist;
using ulw::test::RecordingStore;

const core::WallTime kFirstMedia{std::chrono::seconds(1767225600)};

class PublisherTest : public ::testing::Test {
protected:
    PublisherTest() : store(root.path() / "store") { fs::create_directories(media()); }

    [[nodiscard]] fs::path media() const { return root.path() / "media"; }

    [[nodiscard]] live::PublisherConfig config(std::size_t window = 4,
                                               std::uint32_t target = 2) const {
        return {.stream = *live::StreamId::parse("show"),
                .window = {.target_seconds = target, .max_segments = window},
                .media_dir = media(),
                .outbox = root.path() / "outbox"};
    }

    [[nodiscard]] std::expected<live::Publisher, PublishError> open(std::size_t window = 4,
                                                                    std::uint32_t target = 2) {
        return live::Publisher::open(config(window, target), store, clock);
    }

    // The files ffmpeg would have closed: an init segment and segments first..first+count-1.
    void segments(std::uint32_t epoch, std::uint64_t first, std::uint64_t count) const {
        ulw::test::write_file(media() / init(epoch), "init");
        for (std::uint64_t n = first; n < first + count; ++n) {
            ulw::test::write_file(media() / seg(epoch, n));
        }
    }
    static std::string init(std::uint32_t epoch) {
        return "init_" + std::to_string(epoch) + ".mp4";
    }
    static std::string seg(std::uint32_t epoch, std::uint64_t n) {
        return "seg_" + std::to_string(epoch) + "_" + std::to_string(n) + ".m4s";
    }

    [[nodiscard]] fs::path stored_path(const std::string& name) const {
        return root.path() / "store/objects/live/show" / name;
    }
    [[nodiscard]] std::string stored(const std::string& name) const {
        std::error_code ec;
        std::string text(fs::file_size(stored_path(name), ec), '\0');
        std::ifstream in(stored_path(name), std::ios::binary);
        in.read(text.data(), static_cast<std::streamsize>(text.size()));
        return text;
    }
    [[nodiscard]] bool stored_exists(const std::string& name) const {
        return fs::exists(stored_path(name));
    }
    [[nodiscard]] live::MediaPlaylist stored_playlist() const {
        return live::parse_media_playlist(stored("index.m3u8")).value();
    }

    ulw::test::TempDir root{"ulw-live-publisher"};
    ulw::test::FakeClock clock;
    RecordingStore store;
};

TEST_F(PublisherTest, AStreamWithNothingStoredStartsAtSequenceZeroAndEpochZero) {
    auto publisher = open();
    ASSERT_TRUE(publisher);
    EXPECT_EQ(publisher->next_sequence(), 0U);
    EXPECT_EQ(publisher->epoch(), 0U);
    EXPECT_FALSE(publisher->resumed());
    EXPECT_EQ(store.claims, (std::vector<std::string>{"epoch_0"}));
}

TEST_F(PublisherTest, UploadsTheInitSegmentAndEachSegmentBeforeThePlaylistThatNamesThem) {
    auto publisher = open();
    ASSERT_TRUE(publisher);
    publisher->begin_epoch(kFirstMedia);
    segments(0, 0, 2);
    ASSERT_EQ(publisher->pump(ffmpeg_playlist(0, 0, 2, init(0))).value(), 2U);
    EXPECT_EQ(store.uploads,
              (std::vector<std::string>{"init_0.mp4", "seg_0_0.m4s", "seg_0_1.m4s", "index.m3u8"}));
}

TEST_F(PublisherTest, TheInitSegmentIsUploadedOncePerEpochNotPerSegment) {
    auto publisher = open();
    ASSERT_TRUE(publisher);
    publisher->begin_epoch(kFirstMedia);
    segments(0, 0, 1);
    ASSERT_TRUE(publisher->pump(ffmpeg_playlist(0, 0, 1, init(0))));
    segments(0, 1, 1);
    ASSERT_TRUE(publisher->pump(ffmpeg_playlist(0, 0, 2, init(0))));
    EXPECT_EQ(std::ranges::count(store.uploads, "init_0.mp4"), 1);
    EXPECT_EQ(store.uploads, (std::vector<std::string>{"init_0.mp4", "seg_0_0.m4s", "index.m3u8",
                                                       "seg_0_1.m4s", "index.m3u8"}));
}

TEST_F(PublisherTest, EveryObjectHasTheContentTypeItsPlayersExpect) {
    auto publisher = open();
    ASSERT_TRUE(publisher);
    publisher->begin_epoch(kFirstMedia);
    segments(0, 0, 1);
    ASSERT_TRUE(publisher->pump(ffmpeg_playlist(0, 0, 1, init(0))));
    EXPECT_EQ(store.content_types["init_0.mp4"], "video/mp4");
    EXPECT_EQ(store.content_types["seg_0_0.m4s"], "video/iso.segment");
    EXPECT_EQ(store.content_types["index.m3u8"], "application/vnd.apple.mpegurl");
}

TEST_F(PublisherTest, ThePublishedPlaylistNamesOnlyObjectsThatAreStored) {
    auto publisher = open(3);
    ASSERT_TRUE(publisher);
    publisher->begin_epoch(kFirstMedia);
    segments(0, 0, 5);
    for (std::uint64_t listed = 1; listed <= 5; ++listed) {
        ASSERT_TRUE(publisher->pump(ffmpeg_playlist(0, 0, listed, init(0))));
        for (const live::Segment& segment : stored_playlist().segments) {
            EXPECT_TRUE(stored_exists(segment.uri)) << segment.uri;
            EXPECT_TRUE(stored_exists(segment.init)) << segment.init;
        }
    }
    EXPECT_EQ(stored_playlist().media_sequence, 2U);
}

TEST_F(PublisherTest, TheProgramDateTimesStartAtTheFirstMediaNotAtTheOpen) {
    auto publisher = open();
    ASSERT_TRUE(publisher);
    clock.advance(core::Millis{5000});
    publisher->begin_epoch(kFirstMedia + std::chrono::seconds(7));
    segments(0, 0, 1);
    ASSERT_TRUE(publisher->pump(ffmpeg_playlist(0, 0, 1, init(0))));
    EXPECT_EQ(stored_playlist().segments[0].program_date_time,
              kFirstMedia + std::chrono::seconds(7));
}

TEST_F(PublisherTest, AnUploadedSegmentFileIsRemovedFromTheScratchDirectory) {
    auto publisher = open();
    ASSERT_TRUE(publisher);
    publisher->begin_epoch(kFirstMedia);
    segments(0, 0, 2);
    ASSERT_TRUE(publisher->pump(ffmpeg_playlist(0, 0, 2, init(0))));
    EXPECT_FALSE(fs::exists(media() / seg(0, 0)));
    EXPECT_FALSE(fs::exists(media() / seg(0, 1)));
    EXPECT_TRUE(fs::exists(media() / init(0)));
}

TEST_F(PublisherTest, ASegmentNotYetInFfmpegsPlaylistIsNeverUploaded) {
    auto publisher = open();
    ASSERT_TRUE(publisher);
    publisher->begin_epoch(kFirstMedia);
    segments(0, 0, 3);
    ASSERT_EQ(publisher->pump(ffmpeg_playlist(0, 0, 1, init(0))).value(), 1U);
    EXPECT_FALSE(stored_exists(seg(0, 1)));
    EXPECT_FALSE(stored_exists(seg(0, 2)));
}

TEST_F(PublisherTest, APlaylistFfmpegIsMidwayWritingIsWaitedOutNotFailed) {
    auto publisher = open();
    ASSERT_TRUE(publisher);
    publisher->begin_epoch(kFirstMedia);
    EXPECT_EQ(publisher->pump("").value(), 0U);
    EXPECT_EQ(publisher->pump("#EXTM3U\n#EXTINF:2.0,\n").value(), 0U);
    EXPECT_TRUE(store.uploads.empty());
}

TEST_F(PublisherTest, AFailedSegmentUploadPublishesNothingNewAndTheNextPumpRetriesIt) {
    auto publisher = open();
    ASSERT_TRUE(publisher);
    publisher->begin_epoch(kFirstMedia);
    segments(0, 0, 2);
    ASSERT_TRUE(publisher->pump(ffmpeg_playlist(0, 0, 1, init(0))));
    store.fail_once.insert(seg(0, 1));
    EXPECT_EQ(publisher->pump(ffmpeg_playlist(0, 0, 2, init(0))).error(),
              PublishError::UploadFailed);
    EXPECT_EQ(stored_playlist().segments.size(), 1U);
    EXPECT_EQ(publisher->window().next_sequence(), 1U);
    ASSERT_EQ(publisher->pump(ffmpeg_playlist(0, 0, 2, init(0))).value(), 1U);
    EXPECT_EQ(stored_playlist().segments.size(), 2U);
}

TEST_F(PublisherTest, AFailedPlaylistUploadIsRetriedWithoutSendingTheSegmentsAgain) {
    auto publisher = open();
    ASSERT_TRUE(publisher);
    publisher->begin_epoch(kFirstMedia);
    segments(0, 0, 1);
    store.fail_once.insert("index.m3u8");
    EXPECT_EQ(publisher->pump(ffmpeg_playlist(0, 0, 1, init(0))).error(),
              PublishError::UploadFailed);
    EXPECT_FALSE(stored_exists("index.m3u8"));
    EXPECT_EQ(publisher->pump(ffmpeg_playlist(0, 0, 1, init(0))).value(), 0U);
    EXPECT_EQ(stored_playlist().segments.size(), 1U);
    EXPECT_EQ(std::ranges::count(store.uploads, seg(0, 0)), 1);
}

TEST_F(PublisherTest, ASegmentWhoseFileIsMissingHoldsBackTheOnesAfterIt) {
    auto publisher = open();
    ASSERT_TRUE(publisher);
    publisher->begin_epoch(kFirstMedia);
    ulw::test::write_file(media() / init(0));
    ulw::test::write_file(media() / seg(0, 1));
    // Listed, but seg 0 has no file: seg 1 must not go up ahead of it.
    EXPECT_EQ(publisher->pump(ffmpeg_playlist(0, 0, 2, init(0))).value(), 0U);
    EXPECT_EQ(publisher->window().next_sequence(), 0U);
}

TEST_F(PublisherTest, SegmentsFfmpegDroppedBeforeTheyWentUpAreAnErrorNotAGap) {
    auto publisher = open();
    ASSERT_TRUE(publisher);
    publisher->begin_epoch(kFirstMedia);
    segments(0, 5, 2);
    EXPECT_EQ(publisher->pump(ffmpeg_playlist(0, 5, 2, init(0))).error(),
              PublishError::SegmentsLost);
    EXPECT_TRUE(store.uploads.empty());
}

TEST_F(PublisherTest, AFfmpegPlaylistThatDisagreesWithItsOwnNumberingIsAnError) {
    auto publisher = open();
    ASSERT_TRUE(publisher);
    publisher->begin_epoch(kFirstMedia);
    segments(0, 0, 2);
    std::string text = ffmpeg_playlist(0, 0, 2, init(0));
    text.replace(text.find("seg_0_1"), 7, "seg_0_9");
    ulw::test::write_file(media() / "seg_0_9.m4s");
    EXPECT_EQ(publisher->pump(text).error(), PublishError::PlaylistInconsistent);
}

TEST_F(PublisherTest, ASegmentOfAnotherEpochIsNotOurs) {
    auto publisher = open();
    ASSERT_TRUE(publisher);
    publisher->begin_epoch(kFirstMedia);
    segments(7, 0, 1);
    EXPECT_EQ(publisher->pump(ffmpeg_playlist(7, 0, 1, init(7))).error(),
              PublishError::PlaylistInconsistent);
}

TEST_F(PublisherTest, AnInitNameThatIsNotOneKeySegmentIsRefusedBeforeAnyUpload) {
    auto publisher = open();
    ASSERT_TRUE(publisher);
    publisher->begin_epoch(kFirstMedia);
    segments(0, 0, 1);
    std::string text = ffmpeg_playlist(0, 0, 1, init(0));
    text.replace(text.find("init_0.mp4"), 10, "../init.mp4");
    EXPECT_FALSE(publisher->pump(text));
    EXPECT_TRUE(store.uploads.empty());
}

TEST_F(PublisherTest, ASegmentPastTheTargetDurationIsNotPublishedAndIsAnError) {
    auto publisher = open();
    ASSERT_TRUE(publisher);
    publisher->begin_epoch(kFirstMedia);
    segments(0, 0, 3);
    // 2.4 s rounds to 2, inside the contract; 3.0 s does not.
    std::string text = ffmpeg_playlist(0, 0, 1, init(0), 2.4);
    text += "#EXTINF:3.000000,\nseg_0_1.m4s\n";
    const auto pumped = publisher->pump(text);
    ASSERT_FALSE(pumped);
    EXPECT_EQ(pumped.error(), PublishError::KeyframeIntervalExceeded);
    EXPECT_EQ(stored_playlist().segments.size(), 1U);
    EXPECT_FALSE(stored_exists(seg(0, 1)));
}

TEST_F(PublisherTest, TheTargetDurationOfEveryPublishedPlaylistIsTheSegmentLength) {
    auto publisher = open(4, 2);
    ASSERT_TRUE(publisher);
    publisher->begin_epoch(kFirstMedia);
    segments(0, 0, 2);
    ASSERT_TRUE(publisher->pump(ffmpeg_playlist(0, 0, 2, init(0), 2.499)));
    EXPECT_EQ(stored_playlist().target_seconds, 2U);
}

TEST_F(PublisherTest, FinishUploadsTheLastSegmentsThenEndsThePlaylist) {
    auto publisher = open();
    ASSERT_TRUE(publisher);
    publisher->begin_epoch(kFirstMedia);
    segments(0, 0, 3);
    ASSERT_TRUE(publisher->pump(ffmpeg_playlist(0, 0, 2, init(0))));
    const auto finished = publisher->finish(ffmpeg_playlist(0, 0, 3, init(0)) + "#EXT-X-ENDLIST\n");
    EXPECT_TRUE(finished.ended);
    EXPECT_FALSE(finished.problem);
    const auto playlist = stored_playlist();
    EXPECT_TRUE(playlist.ended);
    EXPECT_EQ(playlist.segments.size(), 3U);
    EXPECT_TRUE(stored_exists(seg(0, 2)));
    EXPECT_EQ(store.uploads.back(), "index.m3u8");
}

TEST_F(PublisherTest, FinishEndsWhatIsVisibleEvenWhenSegmentsWereLost) {
    auto publisher = open();
    ASSERT_TRUE(publisher);
    publisher->begin_epoch(kFirstMedia);
    segments(0, 0, 4);
    ASSERT_TRUE(publisher->pump(ffmpeg_playlist(0, 0, 2, init(0))));
    // ffmpeg's list has moved on past seg 2 and 3, which were never uploaded.
    const auto finished = publisher->finish(ffmpeg_playlist(0, 5, 2, init(0)));
    EXPECT_TRUE(finished.ended);
    EXPECT_EQ(finished.problem, PublishError::SegmentsLost);
    const auto playlist = stored_playlist();
    EXPECT_TRUE(playlist.ended);
    EXPECT_EQ(playlist.segments.size(), 2U);
}

TEST_F(PublisherTest, FinishEndsWhatIsVisibleEvenWhenFfmpegsPlaylistDisagreesWithItself) {
    auto publisher = open();
    ASSERT_TRUE(publisher);
    publisher->begin_epoch(kFirstMedia);
    segments(0, 0, 3);
    ASSERT_TRUE(publisher->pump(ffmpeg_playlist(0, 0, 1, init(0))));
    std::string text = ffmpeg_playlist(0, 0, 3, init(0));
    text.replace(text.find("seg_0_1"), 7, "seg_0_9");
    const auto finished = publisher->finish(text);
    EXPECT_TRUE(finished.ended);
    EXPECT_EQ(finished.problem, PublishError::PlaylistInconsistent);
    EXPECT_TRUE(stored_playlist().ended);
}

TEST_F(PublisherTest, FinishEndsWhatIsVisibleWhenASegmentBrokeTheKeyframeContract) {
    auto publisher = open();
    ASSERT_TRUE(publisher);
    publisher->begin_epoch(kFirstMedia);
    segments(0, 0, 2);
    const std::string text = ffmpeg_playlist(0, 0, 1, init(0)) + "#EXTINF:4.000000,\nseg_0_1.m4s\n";
    const auto finished = publisher->finish(text);
    EXPECT_TRUE(finished.ended);
    EXPECT_EQ(finished.problem, PublishError::KeyframeIntervalExceeded);
    EXPECT_EQ(stored_playlist().segments.size(), 1U);
    EXPECT_TRUE(stored_playlist().ended);
}

TEST_F(PublisherTest, FinishWithoutAnyMediaWritesNoPlaylist) {
    auto publisher = open();
    ASSERT_TRUE(publisher);
    const auto finished = publisher->finish({});
    EXPECT_FALSE(finished.ended);
    EXPECT_FALSE(finished.problem);
    EXPECT_TRUE(store.uploads.empty());
}

TEST_F(PublisherTest, AnEndedStreamIsNotContinued) {
    {
        auto publisher = open();
        ASSERT_TRUE(publisher);
        publisher->begin_epoch(kFirstMedia);
        segments(0, 0, 1);
        ASSERT_TRUE(publisher->finish(ffmpeg_playlist(0, 0, 1, init(0))).ended);
    }
    EXPECT_EQ(open().error(), PublishError::AlreadyEnded);
}

TEST_F(PublisherTest, ARestartContinuesTheSequenceAndStartsANewEpochWithADiscontinuity) {
    {
        auto publisher = open(6);
        ASSERT_TRUE(publisher);
        publisher->begin_epoch(kFirstMedia);
        segments(0, 0, 3);
        ASSERT_TRUE(publisher->pump(ffmpeg_playlist(0, 0, 3, init(0))));
    }
    store.uploads.clear();
    auto publisher = open(6);
    ASSERT_TRUE(publisher);
    EXPECT_TRUE(publisher->resumed());
    EXPECT_EQ(publisher->next_sequence(), 3U);
    EXPECT_EQ(publisher->epoch(), 1U);

    publisher->begin_epoch(kFirstMedia + std::chrono::seconds(30));
    segments(1, 3, 2);
    ASSERT_EQ(publisher->pump(ffmpeg_playlist(1, 3, 2, init(1))).value(), 2U);
    EXPECT_EQ(store.uploads,
              (std::vector<std::string>{"init_1.mp4", "seg_1_3.m4s", "seg_1_4.m4s", "index.m3u8"}));

    const auto playlist = stored_playlist();
    EXPECT_EQ(playlist.media_sequence, 0U);
    ASSERT_EQ(playlist.segments.size(), 5U);
    EXPECT_FALSE(playlist.segments[2].discontinuity);
    EXPECT_TRUE(playlist.segments[3].discontinuity);
    EXPECT_FALSE(playlist.segments[4].discontinuity);
    EXPECT_EQ(playlist.segments[2].init, "init_0.mp4");
    EXPECT_EQ(playlist.segments[3].init, "init_1.mp4");
    // The old segments are still there for a viewer partway through them.
    EXPECT_TRUE(stored_exists(seg(0, 0)));
    EXPECT_TRUE(stored_exists("init_0.mp4"));
}

TEST_F(PublisherTest, TheMediaSequenceNeverGoesBackwardsAcrossRestartsOfASlidingWindow) {
    std::uint64_t last_sequence = 0;
    std::uint64_t next = 0;
    for (std::uint32_t epoch = 0; epoch < 4; ++epoch) {
        auto publisher = open(3);
        ASSERT_TRUE(publisher);
        EXPECT_EQ(publisher->next_sequence(), next);
        EXPECT_EQ(publisher->epoch(), epoch);
        publisher->begin_epoch(kFirstMedia);
        for (std::uint64_t count = 1; count <= 4; ++count) {
            segments(epoch, next, count);
            ASSERT_TRUE(publisher->pump(ffmpeg_playlist(epoch, next, count, init(epoch))));
            const auto playlist = stored_playlist();
            EXPECT_GE(playlist.media_sequence, last_sequence);
            last_sequence = playlist.media_sequence;
        }
        next += 4;
    }
    EXPECT_EQ(next, 16U);
    // Three restarts made three discontinuities, at segments 4, 8 and 12; the window of three
    // holds 13 to 15, so all have slid out.
    EXPECT_EQ(stored_playlist().discontinuity_sequence, 3U);
}

TEST_F(PublisherTest, ARestartWithAnotherSegmentLengthIsRefused) {
    {
        auto publisher = open(4, 2);
        ASSERT_TRUE(publisher);
        publisher->begin_epoch(kFirstMedia);
        segments(0, 0, 1);
        ASSERT_TRUE(publisher->pump(ffmpeg_playlist(0, 0, 1, init(0))));
    }
    EXPECT_EQ(open(4, 4).error(), PublishError::SegmentLengthChanged);
    EXPECT_TRUE(open(4, 2));
}

TEST_F(PublisherTest, ARestartThatFindsTheStoreUnreachableDoesNotStartOver) {
    {
        auto publisher = open();
        ASSERT_TRUE(publisher);
        publisher->begin_epoch(kFirstMedia);
        segments(0, 0, 1);
        ASSERT_TRUE(publisher->pump(ffmpeg_playlist(0, 0, 1, init(0))));
    }
    store.download_error = StorageError::Transient;
    EXPECT_EQ(open().error(), PublishError::StoreUnreadable);
    store.download_error = StorageError::Unauthorized;
    EXPECT_EQ(open().error(), PublishError::StoreUnreadable);
}

TEST_F(PublisherTest, AStoredPlaylistThatIsNotOursIsRefusedNotOverwritten) {
    fs::create_directories(root.path() / "store/objects/live/show");
    std::ofstream(stored_path("index.m3u8")) << "<html>gateway error</html>";
    EXPECT_EQ(open().error(), PublishError::StoredPlaylistInvalid);
}

TEST_F(PublisherTest, AStoredPlaylistWhoseInitSegmentIsNotOursIsRefused) {
    fs::create_directories(root.path() / "store/objects/live/show");
    std::ofstream(stored_path("index.m3u8")) << "#EXTM3U\n#EXT-X-TARGETDURATION:2\n#EXT-X-MAP:URI="
                                                "\"other.mp4\"\n#EXTINF:2.0,\nseg_0_0.m4s\n";
    EXPECT_EQ(open().error(), PublishError::StoredPlaylistInvalid);
}

TEST_F(PublisherTest, StoppingBeforeThePublisherReturnsCanEndTheOldWindow) {
    {
        auto publisher = open();
        ASSERT_TRUE(publisher);
        publisher->begin_epoch(kFirstMedia);
        segments(0, 0, 2);
        ASSERT_TRUE(publisher->pump(ffmpeg_playlist(0, 0, 2, init(0))));
    }
    auto publisher = open();
    ASSERT_TRUE(publisher);
    EXPECT_TRUE(publisher->finish({}).ended);
    const auto playlist = stored_playlist();
    EXPECT_TRUE(playlist.ended);
    EXPECT_EQ(playlist.segments.size(), 2U);
}

TEST_F(PublisherTest, PumpingAloneLeavesTheStreamOpenForAnotherRunToContinue) {
    {
        auto publisher = open();
        ASSERT_TRUE(publisher);
        publisher->begin_epoch(kFirstMedia);
        segments(0, 0, 2);
        ASSERT_TRUE(publisher->pump(ffmpeg_playlist(0, 0, 2, init(0))));
    }
    EXPECT_FALSE(stored_playlist().ended);
    auto again = open();
    ASSERT_TRUE(again);
    EXPECT_TRUE(again->resumed());
}

// Two packagers on one stream: the one that started later holds the newer epoch.

TEST_F(PublisherTest, EachPackagerClaimsAnEpochOfItsOwn) {
    auto older = open();
    auto newer = open();
    ASSERT_TRUE(older && newer);
    EXPECT_EQ(older->epoch(), 0U);
    EXPECT_EQ(newer->epoch(), 1U);
    EXPECT_EQ(store.claims, (std::vector<std::string>{"epoch_0", "epoch_1"}));
}

TEST_F(PublisherTest, TheOlderPackagerStopsAtItsNextPlaylistWriteAndTheNewerOnesIsUntouched) {
    auto older = open();
    ASSERT_TRUE(older);
    older->begin_epoch(kFirstMedia);
    segments(0, 0, 2);
    ASSERT_TRUE(older->pump(ffmpeg_playlist(0, 0, 1, init(0))));

    auto newer = open();
    ASSERT_TRUE(newer);
    ASSERT_TRUE(newer->resumed());
    newer->begin_epoch(kFirstMedia + std::chrono::seconds(10));
    segments(1, 1, 2);
    ASSERT_TRUE(newer->pump(ffmpeg_playlist(1, 1, 2, init(1))));
    const std::string newer_playlist = stored("index.m3u8");

    // The older one has another segment ready; its segment and playlist are not written.
    store.uploads.clear();
    const auto pumped = older->pump(ffmpeg_playlist(0, 0, 2, init(0)));
    ASSERT_FALSE(pumped);
    EXPECT_EQ(pumped.error(), PublishError::Superseded);
    EXPECT_EQ(stored("index.m3u8"), newer_playlist);
    // Its segment went up under an epoch-0 name, which no playlist of the newer one lists.
    for (const live::Segment& segment : stored_playlist().segments) {
        EXPECT_NE(segment.uri, seg(0, 1));
    }
    EXPECT_EQ(std::ranges::count(store.uploads, "index.m3u8"), 0);
}

TEST_F(PublisherTest, ASupersededPackagerDoesNotEndTheStream) {
    auto older = open();
    ASSERT_TRUE(older);
    older->begin_epoch(kFirstMedia);
    segments(0, 0, 2);
    ASSERT_TRUE(older->pump(ffmpeg_playlist(0, 0, 1, init(0))));
    auto newer = open();
    ASSERT_TRUE(newer);
    const auto finished = older->finish(ffmpeg_playlist(0, 0, 2, init(0)));
    EXPECT_FALSE(finished.ended);
    EXPECT_EQ(finished.problem, PublishError::Superseded);
    EXPECT_FALSE(stored_playlist().ended);
}

// A playlist of `count` segments of `epoch`, from `first`, as another packager left it.
void write_stored_playlist(const fs::path& file, std::uint32_t epoch, std::uint64_t first,
                           std::uint64_t count) {
    live::MediaPlaylist playlist;
    playlist.target_seconds = 2;
    playlist.media_sequence = first;
    for (std::uint64_t n = first; n < first + count; ++n) {
        playlist.segments.push_back(
            {.uri = "seg_" + std::to_string(epoch) + "_" + std::to_string(n) + ".m4s",
             .duration = live::Micros{2'000'000},
             .init = "init_" + std::to_string(epoch) + ".mp4",
             .discontinuity = false,
             .program_date_time = std::nullopt});
    }
    fs::create_directories(file.parent_path());
    std::ofstream(file) << live::render_media_playlist(playlist);
}

TEST_F(PublisherTest, ThePlaylistIsReadAgainAfterTheClaimSoNothingGoesBackwards) {
    write_stored_playlist(stored_path("index.m3u8"), 0, 0, 3);
    // The previous writer published two more segments after this one had read the playlist
    // and before its claim went in.
    store.after_claim = [&](const std::string&) {
        write_stored_playlist(stored_path("index.m3u8"), 0, 0, 5);
    };
    auto publisher = open(8);
    ASSERT_TRUE(publisher);
    EXPECT_EQ(publisher->epoch(), 1U);
    EXPECT_EQ(publisher->next_sequence(), 5U);
}

TEST_F(PublisherTest, AnEpochThatPublishedDuringTheClaimSendsTheClaimHigher) {
    write_stored_playlist(stored_path("index.m3u8"), 0, 0, 3);
    int claims = 0;
    // Someone else claimed the next epoch and published under it in the same instant.
    store.after_claim = [&](const std::string&) {
        if (claims++ == 0) {
            write_stored_playlist(stored_path("index.m3u8"), 1, 0, 4);
            ulw::test::write_file(stored_path("epoch_1"));
            ulw::test::write_file(stored_path("epoch_2"));
        }
    };
    auto publisher = open(8);
    ASSERT_TRUE(publisher);
    EXPECT_GT(publisher->epoch(), 1U);
    EXPECT_EQ(publisher->next_sequence(), 4U);
}

TEST_F(PublisherTest, AClaimLeftByARunThatNeverPublishedIsSteppedOver) {
    {
        auto dead = open();
        ASSERT_TRUE(dead);
    }
    auto next = open();
    ASSERT_TRUE(next);
    EXPECT_EQ(next->epoch(), 1U);
    EXPECT_FALSE(next->resumed());
}

TEST_F(PublisherTest, AStoreThatCannotSayWhetherANewerEpochExistsGetsNoPlaylistWrite) {
    auto publisher = open();
    ASSERT_TRUE(publisher);
    publisher->begin_epoch(kFirstMedia);
    segments(0, 0, 1);
    store.size_error = StorageError::Transient;
    EXPECT_EQ(publisher->pump(ffmpeg_playlist(0, 0, 1, init(0))).error(),
              PublishError::UploadFailed);
    EXPECT_FALSE(stored_exists("index.m3u8"));
    store.size_error.reset();
    EXPECT_TRUE(publisher->pump(ffmpeg_playlist(0, 0, 1, init(0))));
    EXPECT_TRUE(stored_exists("index.m3u8"));
}

TEST_F(PublisherTest, AClaimTheStoreRefusesForAnotherReasonFailsTheOpen) {
    fs::create_directories(root.path() / "store/objects/live");
    // A file where the stream's directory should be.
    std::ofstream(root.path() / "store/objects/live/show") << "x";
    EXPECT_FALSE(open());
}

} // namespace
