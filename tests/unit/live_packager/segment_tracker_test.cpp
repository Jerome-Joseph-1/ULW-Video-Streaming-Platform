#include "segment_tracker.hpp"

#include <gtest/gtest.h>
#include <map>
#include <string>

namespace {

using live::ScanError;
using live::SegmentTracker;

std::string playlist(std::uint64_t first, std::uint64_t count, bool ended = false) {
    std::string text = "#EXTM3U\n#EXT-X-VERSION:7\n#EXT-X-TARGETDURATION:2\n"
                       "#EXT-X-MEDIA-SEQUENCE:" +
                       std::to_string(first) + "\n#EXT-X-MAP:URI=\"init_0.mp4\"\n";
    for (std::uint64_t n = first; n < first + count; ++n) {
        text += "#EXTINF:2.000000,\nseg_0_" + std::to_string(n) + ".m4s\n";
    }
    return ended ? text + "#EXT-X-ENDLIST\n" : text;
}

class Directory {
public:
    void write(std::uint64_t n, std::uint64_t bytes = 1000) {
        files_["seg_0_" + std::to_string(n) + ".m4s"] = bytes;
    }
    [[nodiscard]] live::FileSize lookup() const {
        return [this](std::string_view name) -> std::optional<std::uint64_t> {
            const auto it = files_.find(std::string(name));
            return it == files_.end() ? std::nullopt : std::optional<std::uint64_t>(it->second);
        };
    }

private:
    std::map<std::string, std::uint64_t> files_;
};

TEST(SegmentTracker, ASegmentNamedForAnotherEpochIsNotOurs) {
    Directory dir;
    dir.write(0);
    std::string text = playlist(0, 1);
    text.replace(text.find("seg_0_0"), 7, "seg_9_0");
    const auto ready = SegmentTracker(0, 0).scan(text, dir.lookup());
    ASSERT_FALSE(ready);
    EXPECT_EQ(ready.error(), ScanError::Inconsistent);
}

TEST(ListedEnd, IsOnePastTheNewestSequenceListed) {
    EXPECT_EQ(live::listed_end(playlist(5, 3)), 8U);
    EXPECT_EQ(live::listed_end(playlist(5, 0)), 5U);
}

TEST(ListedEnd, IsNothingForTextThatIsNotAPlaylist) {
    EXPECT_FALSE(live::listed_end(""));
    EXPECT_FALSE(live::listed_end("#EXTM3U\n#EXTINF:2.0,\n"));
}

TEST(SegmentTracker, HandsOutEveryListedSegmentWhoseFileIsWhole) {
    Directory dir;
    dir.write(0);
    dir.write(1);
    const auto ready = SegmentTracker(0, 0).scan(playlist(0, 2), dir.lookup());
    ASSERT_TRUE(ready);
    ASSERT_EQ(ready->size(), 2U);
    EXPECT_EQ((*ready)[0].sequence, 0U);
    EXPECT_EQ((*ready)[0].uri, "seg_0_0.m4s");
    EXPECT_EQ((*ready)[0].init, "init_0.mp4");
    EXPECT_EQ((*ready)[0].duration, live::Micros{2'000'000});
    EXPECT_EQ((*ready)[1].sequence, 1U);
}

TEST(SegmentTracker, ASegmentFfmpegIsStillWritingIsNotHandedOut) {
    // Not in ffmpeg's playlist yet: the file exists under its temporary name only, or is
    // growing. Whatever the directory holds, only what the playlist lists is complete.
    Directory dir;
    dir.write(0);
    dir.write(1);
    const auto ready = SegmentTracker(0, 0).scan(playlist(0, 1), dir.lookup());
    ASSERT_TRUE(ready);
    ASSERT_EQ(ready->size(), 1U);
    EXPECT_EQ((*ready)[0].sequence, 0U);
}

TEST(SegmentTracker, AListedSegmentWithoutAFileWaitsAndHoldsBackTheOnesAfterIt) {
    Directory dir;
    dir.write(1);
    const auto ready = SegmentTracker(0, 0).scan(playlist(0, 2), dir.lookup());
    ASSERT_TRUE(ready);
    EXPECT_TRUE(ready->empty());
}

TEST(SegmentTracker, AnEmptyFileIsNotComplete) {
    Directory dir;
    dir.write(0, 0);
    const auto ready = SegmentTracker(0, 0).scan(playlist(0, 1), dir.lookup());
    ASSERT_TRUE(ready);
    EXPECT_TRUE(ready->empty());
}

TEST(SegmentTracker, NothingIsHandedOutTwiceOnceItIsHandled) {
    Directory dir;
    dir.write(0);
    dir.write(1);
    SegmentTracker tracker(0, 0);
    tracker.handled(0);
    const auto ready = tracker.scan(playlist(0, 2), dir.lookup());
    ASSERT_TRUE(ready);
    ASSERT_EQ(ready->size(), 1U);
    EXPECT_EQ((*ready)[0].sequence, 1U);
    EXPECT_EQ(tracker.next(), 1U);
}

TEST(SegmentTracker, ScanningAgainWithoutHandlingHandsTheSameSegmentsAgain) {
    Directory dir;
    dir.write(0);
    const SegmentTracker tracker(0, 0);
    EXPECT_EQ(tracker.scan(playlist(0, 1), dir.lookup())->size(), 1U);
    EXPECT_EQ(tracker.scan(playlist(0, 1), dir.lookup())->size(), 1U);
}

TEST(SegmentTracker, StartsAtTheSequenceItWasGiven) {
    Directory dir;
    dir.write(40);
    dir.write(41);
    const auto ready = SegmentTracker(0, 40).scan(playlist(40, 2), dir.lookup());
    ASSERT_TRUE(ready);
    ASSERT_EQ(ready->size(), 2U);
    EXPECT_EQ((*ready)[0].sequence, 40U);
}

TEST(SegmentTracker, SkipsSegmentsAlreadyHandledThatFfmpegStillLists) {
    Directory dir;
    for (std::uint64_t n = 5; n < 9; ++n) {
        dir.write(n);
    }
    SegmentTracker tracker(0, 7);
    const auto ready = tracker.scan(playlist(5, 4), dir.lookup());
    ASSERT_TRUE(ready);
    ASSERT_EQ(ready->size(), 2U);
    EXPECT_EQ((*ready)[0].sequence, 7U);
    EXPECT_EQ((*ready)[1].sequence, 8U);
}

TEST(SegmentTracker, ReportsSegmentsFfmpegListedAndDroppedBeforeTheyWereHandedOut) {
    Directory dir;
    dir.write(10);
    const auto ready = SegmentTracker(0, 4).scan(playlist(10, 1), dir.lookup());
    ASSERT_FALSE(ready);
    EXPECT_EQ(ready.error(), ScanError::Lost);
}

TEST(SegmentTracker, ReportsAPlaylistThatEndsBeforeWhatWasAlreadyHandedOut) {
    Directory dir;
    dir.write(0);
    SegmentTracker tracker(0, 0);
    tracker.handled(5);
    const auto ready = tracker.scan(playlist(0, 3), dir.lookup());
    ASSERT_FALSE(ready);
    EXPECT_EQ(ready.error(), ScanError::Inconsistent);
}

TEST(SegmentTracker, ReportsASegmentNamedOtherThanItsSequenceNumberSays) {
    Directory dir;
    dir.write(0);
    // Sequence 3 with the name of segment 0: this list is not the run being followed.
    std::string text = playlist(3, 1);
    text.replace(text.find("seg_0_3"), 7, "seg_0_0");
    const auto ready = SegmentTracker(0, 3).scan(text, dir.lookup());
    ASSERT_FALSE(ready);
    EXPECT_EQ(ready.error(), ScanError::Inconsistent);
}

TEST(SegmentTracker, AHalfWrittenPlaylistIsUnreadableRatherThanEmpty) {
    Directory dir;
    EXPECT_EQ(SegmentTracker(0, 0).scan("", dir.lookup()).error(), ScanError::Unreadable);
    EXPECT_EQ(SegmentTracker(0, 0).scan("#EXTM3U\n#EXTINF:2.0,\n", dir.lookup()).error(),
              ScanError::Unreadable);
}

TEST(SegmentTracker, ThePlaylistFfmpegEndsStillHandsOutItsLastSegments) {
    Directory dir;
    dir.write(0);
    dir.write(1);
    const auto ready = SegmentTracker(0, 0).scan(playlist(0, 2, true), dir.lookup());
    ASSERT_TRUE(ready);
    EXPECT_EQ(ready->size(), 2U);
}

} // namespace
