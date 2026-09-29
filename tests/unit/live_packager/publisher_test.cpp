#include "infra/storage/fs_transfer.hpp"

#include "publisher.hpp"
#include "support/fake_clock.hpp"
#include "support/temp_dir.hpp"

#include <algorithm>
#include <fstream>
#include <gtest/gtest.h>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
using core::ports::StorageError;
using live::PublishError;

// The filesystem store, plus a record of every upload in order and faults to inject.
class RecordingStore final : public core::ports::IObjectTransfer {
public:
    explicit RecordingStore(const fs::path& root) : inner_(root) {}

    std::expected<std::uint64_t, StorageError> size(const core::StorageKey& key) override {
        return inner_.size(key);
    }
    std::expected<std::uint64_t, StorageError> download(const core::StorageKey& key,
                                                        const fs::path& destination) override {
        if (download_error) {
            return std::unexpected(*download_error);
        }
        return inner_.download(key, destination);
    }
    std::expected<void, StorageError> upload(const fs::path& source, const core::StorageKey& key,
                                             const core::ContentType& type) override {
        const std::string name = key.str().substr(key.str().rfind('/') + 1);
        if (fail_once.erase(name) != 0) {
            return std::unexpected(StorageError::Transient);
        }
        uploads.push_back(name);
        content_types[name] = std::string(type.view());
        return inner_.upload(source, key, type);
    }

    std::vector<std::string> uploads;
    std::map<std::string, std::string> content_types;
    std::set<std::string> fail_once;
    std::optional<StorageError> download_error;

private:
    infra::storage::FsTransfer inner_;
};

std::string ffmpeg_playlist(std::uint64_t first, std::uint64_t count, std::string_view init,
                            double seconds = 2.0) {
    std::ostringstream text;
    text << "#EXTM3U\n#EXT-X-VERSION:7\n#EXT-X-TARGETDURATION:2\n#EXT-X-MEDIA-SEQUENCE:" << first
         << "\n#EXT-X-INDEPENDENT-SEGMENTS\n#EXT-X-MAP:URI=\"" << init << "\"\n";
    for (std::uint64_t n = first; n < first + count; ++n) {
        text.precision(6);
        text << "#EXTINF:" << std::fixed << seconds << ",\nseg_" << n << ".m4s\n";
    }
    return text.str();
}

class PublisherTest : public ::testing::Test {
protected:
    PublisherTest() : store(root.path() / "store") { fs::create_directories(media()); }

    [[nodiscard]] fs::path media() const { return root.path() / "media"; }

    [[nodiscard]] live::PublisherConfig config(std::size_t window = 4) const {
        return {.stream = *live::StreamId::parse("show"),
                .window = {.target_seconds = 2, .max_segments = window},
                .media_dir = media(),
                .outbox = root.path() / "outbox"};
    }

    [[nodiscard]] std::expected<live::Publisher, PublishError> open(std::size_t window = 4) {
        return live::Publisher::open(config(window), store, clock);
    }

    void write(const std::string& name, std::string_view bytes = "bytes") const {
        std::ofstream(media() / name, std::ios::binary) << bytes;
    }
    void segments(std::uint64_t from, std::uint64_t count, const std::string& init) const {
        write(init, "init");
        for (std::uint64_t n = from; n < from + count; ++n) {
            write("seg_" + std::to_string(n) + ".m4s");
        }
    }

    [[nodiscard]] std::string stored(const std::string& name) const {
        const fs::path file = root.path() / "store/objects/live/show" / name;
        std::error_code ec;
        std::string text(fs::file_size(file, ec), '\0');
        std::ifstream in(file, std::ios::binary);
        in.read(text.data(), static_cast<std::streamsize>(text.size()));
        return text;
    }
    [[nodiscard]] bool stored_exists(const std::string& name) const {
        return fs::exists(root.path() / "store/objects/live/show" / name);
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
}

TEST_F(PublisherTest, UploadsTheInitSegmentAndEachSegmentBeforeThePlaylistThatNamesThem) {
    auto publisher = open();
    ASSERT_TRUE(publisher);
    publisher->begin_epoch();
    segments(0, 2, "init_0.mp4");
    ASSERT_EQ(publisher->pump(ffmpeg_playlist(0, 2, "init_0.mp4")).value(), 2U);
    EXPECT_EQ(store.uploads,
              (std::vector<std::string>{"init_0.mp4", "seg_0.m4s", "seg_1.m4s", "index.m3u8"}));
}

TEST_F(PublisherTest, TheInitSegmentIsUploadedOncePerEpochNotPerSegment) {
    auto publisher = open();
    ASSERT_TRUE(publisher);
    publisher->begin_epoch();
    segments(0, 1, "init_0.mp4");
    ASSERT_TRUE(publisher->pump(ffmpeg_playlist(0, 1, "init_0.mp4")));
    segments(1, 1, "init_0.mp4");
    ASSERT_TRUE(publisher->pump(ffmpeg_playlist(0, 2, "init_0.mp4")));
    EXPECT_EQ(std::ranges::count(store.uploads, "init_0.mp4"), 1);
    EXPECT_EQ(store.uploads, (std::vector<std::string>{"init_0.mp4", "seg_0.m4s", "index.m3u8",
                                                       "seg_1.m4s", "index.m3u8"}));
}

TEST_F(PublisherTest, EveryObjectHasTheContentTypeItsPlayersExpect) {
    auto publisher = open();
    ASSERT_TRUE(publisher);
    publisher->begin_epoch();
    segments(0, 1, "init_0.mp4");
    ASSERT_TRUE(publisher->pump(ffmpeg_playlist(0, 1, "init_0.mp4")));
    EXPECT_EQ(store.content_types["init_0.mp4"], "video/mp4");
    EXPECT_EQ(store.content_types["seg_0.m4s"], "video/iso.segment");
    EXPECT_EQ(store.content_types["index.m3u8"], "application/vnd.apple.mpegurl");
}

TEST_F(PublisherTest, ThePublishedPlaylistNamesOnlyObjectsThatAreStored) {
    auto publisher = open(3);
    ASSERT_TRUE(publisher);
    publisher->begin_epoch();
    segments(0, 5, "init_0.mp4");
    for (std::uint64_t listed = 1; listed <= 5; ++listed) {
        ASSERT_TRUE(publisher->pump(ffmpeg_playlist(0, listed, "init_0.mp4")));
        const auto playlist = stored_playlist();
        for (const live::Segment& segment : playlist.segments) {
            EXPECT_TRUE(stored_exists(segment.uri)) << segment.uri;
            EXPECT_TRUE(stored_exists(segment.init)) << segment.init;
        }
    }
    EXPECT_EQ(stored_playlist().media_sequence, 2U);
}

TEST_F(PublisherTest, AnUploadedSegmentFileIsRemovedFromTheScratchDirectory) {
    auto publisher = open();
    ASSERT_TRUE(publisher);
    publisher->begin_epoch();
    segments(0, 2, "init_0.mp4");
    ASSERT_TRUE(publisher->pump(ffmpeg_playlist(0, 2, "init_0.mp4")));
    EXPECT_FALSE(fs::exists(media() / "seg_0.m4s"));
    EXPECT_FALSE(fs::exists(media() / "seg_1.m4s"));
    EXPECT_TRUE(fs::exists(media() / "init_0.mp4"));
}

TEST_F(PublisherTest, ASegmentNotYetInFfmpegsPlaylistIsNeverUploaded) {
    auto publisher = open();
    ASSERT_TRUE(publisher);
    publisher->begin_epoch();
    segments(0, 3, "init_0.mp4");
    ASSERT_EQ(publisher->pump(ffmpeg_playlist(0, 1, "init_0.mp4")).value(), 1U);
    EXPECT_FALSE(stored_exists("seg_1.m4s"));
    EXPECT_FALSE(stored_exists("seg_2.m4s"));
}

TEST_F(PublisherTest, APlaylistFfmpegIsMidwayWritingIsWaitedOutNotFailed) {
    auto publisher = open();
    ASSERT_TRUE(publisher);
    publisher->begin_epoch();
    EXPECT_EQ(publisher->pump("").value(), 0U);
    EXPECT_EQ(publisher->pump("#EXTM3U\n#EXTINF:2.0,\n").value(), 0U);
    EXPECT_TRUE(store.uploads.empty());
}

TEST_F(PublisherTest, AFailedSegmentUploadPublishesNothingNewAndTheNextPumpRetriesIt) {
    auto publisher = open();
    ASSERT_TRUE(publisher);
    publisher->begin_epoch();
    segments(0, 2, "init_0.mp4");
    ASSERT_TRUE(publisher->pump(ffmpeg_playlist(0, 1, "init_0.mp4")));
    store.fail_once.insert("seg_1.m4s");
    EXPECT_EQ(publisher->pump(ffmpeg_playlist(0, 2, "init_0.mp4")).error(),
              PublishError::UploadFailed);
    EXPECT_EQ(stored_playlist().segments.size(), 1U);
    EXPECT_EQ(publisher->window().next_sequence(), 1U);
    ASSERT_EQ(publisher->pump(ffmpeg_playlist(0, 2, "init_0.mp4")).value(), 1U);
    EXPECT_EQ(stored_playlist().segments.size(), 2U);
}

TEST_F(PublisherTest, AFailedPlaylistUploadIsRetriedWithoutSendingTheSegmentsAgain) {
    auto publisher = open();
    ASSERT_TRUE(publisher);
    publisher->begin_epoch();
    segments(0, 1, "init_0.mp4");
    store.fail_once.insert("index.m3u8");
    EXPECT_EQ(publisher->pump(ffmpeg_playlist(0, 1, "init_0.mp4")).error(),
              PublishError::UploadFailed);
    EXPECT_FALSE(stored_exists("index.m3u8"));
    EXPECT_EQ(publisher->pump(ffmpeg_playlist(0, 1, "init_0.mp4")).value(), 0U);
    EXPECT_EQ(stored_playlist().segments.size(), 1U);
    EXPECT_EQ(std::ranges::count(store.uploads, "seg_0.m4s"), 1);
}

TEST_F(PublisherTest, ASegmentWhoseFileIsMissingHoldsBackTheOnesAfterIt) {
    auto publisher = open();
    ASSERT_TRUE(publisher);
    publisher->begin_epoch();
    write("init_0.mp4");
    write("seg_1.m4s");
    // Listed, but seg_0 has no file: seg_1 must not go up ahead of it.
    EXPECT_EQ(publisher->pump(ffmpeg_playlist(0, 2, "init_0.mp4")).value(), 0U);
    EXPECT_EQ(publisher->window().next_sequence(), 0U);
}

TEST_F(PublisherTest, SegmentsFfmpegDroppedBeforeTheyWentUpAreAnErrorNotAGap) {
    auto publisher = open();
    ASSERT_TRUE(publisher);
    publisher->begin_epoch();
    segments(5, 2, "init_0.mp4");
    EXPECT_EQ(publisher->pump(ffmpeg_playlist(5, 2, "init_0.mp4")).error(),
              PublishError::SegmentsLost);
    EXPECT_TRUE(store.uploads.empty());
}

TEST_F(PublisherTest, AFfmpegPlaylistThatDisagreesWithItsOwnNumberingIsAnError) {
    auto publisher = open();
    ASSERT_TRUE(publisher);
    publisher->begin_epoch();
    segments(0, 2, "init_0.mp4");
    std::string text = ffmpeg_playlist(0, 2, "init_0.mp4");
    text.replace(text.find("seg_1"), 5, "seg_9");
    write("seg_9.m4s");
    EXPECT_EQ(publisher->pump(text).error(), PublishError::PlaylistInconsistent);
}

TEST_F(PublisherTest, ASegmentNameThatIsNotOneKeySegmentIsRefusedBeforeAnyUpload) {
    auto publisher = open();
    ASSERT_TRUE(publisher);
    publisher->begin_epoch();
    segments(0, 1, "init_0.mp4");
    std::string text = ffmpeg_playlist(0, 1, "init_0.mp4");
    text.replace(text.find("init_0.mp4"), 10, "../init.mp4");
    EXPECT_FALSE(publisher->pump(text));
    EXPECT_TRUE(store.uploads.empty());
}

TEST_F(PublisherTest, CountsSegmentsLongerThanTheTargetDuration) {
    auto publisher = open();
    ASSERT_TRUE(publisher);
    publisher->begin_epoch();
    segments(0, 2, "init_0.mp4");
    ASSERT_TRUE(publisher->pump(ffmpeg_playlist(0, 2, "init_0.mp4", 3.0)));
    EXPECT_EQ(publisher->overlong_segments(), 2U);
}

TEST_F(PublisherTest, FinishUploadsTheLastSegmentsThenEndsThePlaylist) {
    auto publisher = open();
    ASSERT_TRUE(publisher);
    publisher->begin_epoch();
    segments(0, 3, "init_0.mp4");
    ASSERT_TRUE(publisher->pump(ffmpeg_playlist(0, 2, "init_0.mp4")));
    ASSERT_TRUE(publisher->finish(ffmpeg_playlist(0, 3, "init_0.mp4") + "#EXT-X-ENDLIST\n"));
    const auto playlist = stored_playlist();
    EXPECT_TRUE(playlist.ended);
    EXPECT_EQ(playlist.segments.size(), 3U);
    EXPECT_TRUE(stored_exists("seg_2.m4s"));
    EXPECT_EQ(store.uploads.back(), "index.m3u8");
}

TEST_F(PublisherTest, FinishWithoutAnyMediaWritesNoPlaylist) {
    auto publisher = open();
    ASSERT_TRUE(publisher);
    ASSERT_TRUE(publisher->finish({}));
    EXPECT_TRUE(store.uploads.empty());
}

TEST_F(PublisherTest, AnEndedStreamIsNotContinued) {
    {
        auto publisher = open();
        ASSERT_TRUE(publisher);
        publisher->begin_epoch();
        segments(0, 1, "init_0.mp4");
        ASSERT_TRUE(publisher->finish(ffmpeg_playlist(0, 1, "init_0.mp4")));
    }
    EXPECT_EQ(open().error(), PublishError::AlreadyEnded);
}

TEST_F(PublisherTest, ARestartContinuesTheSequenceAndStartsANewEpochWithADiscontinuity) {
    {
        auto publisher = open(6);
        ASSERT_TRUE(publisher);
        publisher->begin_epoch();
        segments(0, 3, "init_0.mp4");
        ASSERT_TRUE(publisher->pump(ffmpeg_playlist(0, 3, "init_0.mp4")));
    }
    store.uploads.clear();
    auto publisher = open(6);
    ASSERT_TRUE(publisher);
    EXPECT_TRUE(publisher->resumed());
    EXPECT_EQ(publisher->next_sequence(), 3U);
    EXPECT_EQ(publisher->epoch(), 1U);

    clock.advance(core::Millis{30'000});
    publisher->begin_epoch();
    segments(3, 2, "init_1.mp4");
    ASSERT_EQ(publisher->pump(ffmpeg_playlist(3, 2, "init_1.mp4")).value(), 2U);
    EXPECT_EQ(store.uploads,
              (std::vector<std::string>{"init_1.mp4", "seg_3.m4s", "seg_4.m4s", "index.m3u8"}));

    const auto playlist = stored_playlist();
    EXPECT_EQ(playlist.media_sequence, 0U);
    ASSERT_EQ(playlist.segments.size(), 5U);
    EXPECT_FALSE(playlist.segments[2].discontinuity);
    EXPECT_TRUE(playlist.segments[3].discontinuity);
    EXPECT_FALSE(playlist.segments[4].discontinuity);
    EXPECT_EQ(playlist.segments[2].init, "init_0.mp4");
    EXPECT_EQ(playlist.segments[3].init, "init_1.mp4");
    // The old segments are still there for a viewer partway through them.
    EXPECT_TRUE(stored_exists("seg_0.m4s"));
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
        publisher->begin_epoch();
        const std::string init = "init_" + std::to_string(epoch) + ".mp4";
        for (std::uint64_t count = 1; count <= 4; ++count) {
            segments(next, count, init);
            ASSERT_TRUE(publisher->pump(ffmpeg_playlist(next, count, init)));
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

TEST_F(PublisherTest, ARestartThatFindsTheStoreUnreachableDoesNotStartOver) {
    {
        auto publisher = open();
        ASSERT_TRUE(publisher);
        publisher->begin_epoch();
        segments(0, 1, "init_0.mp4");
        ASSERT_TRUE(publisher->pump(ffmpeg_playlist(0, 1, "init_0.mp4")));
    }
    store.download_error = StorageError::Transient;
    EXPECT_EQ(open().error(), PublishError::StoreUnreadable);
    store.download_error = StorageError::Unauthorized;
    EXPECT_EQ(open().error(), PublishError::StoreUnreadable);
}

TEST_F(PublisherTest, AStoredPlaylistThatIsNotOursIsRefusedNotOverwritten) {
    fs::create_directories(root.path() / "store/objects/live/show");
    std::ofstream(root.path() / "store/objects/live/show/index.m3u8")
        << "<html>gateway error</html>";
    EXPECT_EQ(open().error(), PublishError::StoredPlaylistInvalid);
}

TEST_F(PublisherTest, AStoredPlaylistWhoseInitSegmentIsNotOursIsRefused) {
    fs::create_directories(root.path() / "store/objects/live/show");
    std::ofstream(root.path() / "store/objects/live/show/index.m3u8")
        << "#EXTM3U\n#EXT-X-TARGETDURATION:2\n#EXT-X-MAP:URI=\"other.mp4\"\n#EXTINF:2.0,\nseg_0."
           "m4s\n";
    EXPECT_EQ(open().error(), PublishError::StoredPlaylistInvalid);
}

TEST_F(PublisherTest, StoppingBeforeThePublisherReturnsEndsTheOldWindow) {
    {
        auto publisher = open();
        ASSERT_TRUE(publisher);
        publisher->begin_epoch();
        segments(0, 2, "init_0.mp4");
        ASSERT_TRUE(publisher->pump(ffmpeg_playlist(0, 2, "init_0.mp4")));
    }
    auto publisher = open();
    ASSERT_TRUE(publisher);
    ASSERT_TRUE(publisher->finish({}));
    const auto playlist = stored_playlist();
    EXPECT_TRUE(playlist.ended);
    EXPECT_EQ(playlist.segments.size(), 2U);
}

} // namespace
