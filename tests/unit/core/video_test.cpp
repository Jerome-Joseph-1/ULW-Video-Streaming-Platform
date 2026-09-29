#include "core/errors/domain_error.hpp"
#include "core/models/ids.hpp"
#include "core/models/video.hpp"
#include "core/util/time.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <gtest/gtest.h>
#include <limits>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

namespace {

using core::DomainError;
using core::Millis;
using core::UserId;
using core::Video;
using core::VideoId;
using core::VideoRecord;
using core::VideoState;

constexpr std::string_view kTitle = "Launch keynote";
constexpr std::string_view kReason = "decoder rejected stream";
constexpr Millis kDuration{90'000};
// What apply() passes. Distinct from the stored values above, so a rejected call that writes
// before it checks shows up as a changed field.
constexpr std::string_view kNewReason = "worker lost";
constexpr Millis kNewDuration{120'000};
// Not 0, so a transition that resets the version instead of incrementing it is caught.
constexpr std::uint64_t kVersion = 5;

VideoId video_id() {
    return VideoId::parse("0192f3c4-7a1b-7c2d-8e3f-0123456789ab").value();
}

UserId owner() {
    return UserId::parse("auth0|5f7c8ec7c33c6c004bbafe82").value();
}

VideoRecord record_in(VideoState state) {
    VideoRecord record{.id = video_id(),
                       .owner = owner(),
                       .title = std::string(kTitle),
                       .state = state,
                       .version = kVersion,
                       .error_reason = std::nullopt,
                       .duration = std::nullopt};
    if (state == VideoState::Ready) {
        record.duration = kDuration;
    }
    if (state == VideoState::Failed) {
        record.error_reason = std::string(kReason);
    }
    return record;
}

Video video_in(VideoState state) {
    return Video::rehydrate(record_in(state)).value();
}

auto observe(const Video& video) {
    return std::tuple{
        video.id().to_string(),
        std::string(video.owner().view()),
        std::string(video.title()),
        video.state(),
        video.version(),
        video.error_reason().transform([](std::string_view r) { return std::string(r); }),
        video.duration().transform([](Millis d) { return d.count(); })};
}

enum class Op : std::uint8_t { StartProcessing, MarkReady, MarkFailed };

std::expected<void, DomainError> apply(Video& video, Op op) {
    switch (op) {
    case Op::StartProcessing:
        return video.start_processing();
    case Op::MarkReady:
        return video.mark_ready(kNewDuration);
    case Op::MarkFailed:
        return video.mark_failed(std::string(kNewReason));
    }
    std::unreachable();
}

std::string name_of(VideoState state, Op op) {
    constexpr std::array<std::string_view, 5> kStates{"Init", "Uploading", "Processing", "Ready",
                                                      "Failed"};
    constexpr std::array<std::string_view, 3> kOps{"StartProcessing", "MarkReady", "MarkFailed"};
    return std::string(kStates.at(static_cast<std::size_t>(state))) + "_" +
           std::string(kOps.at(static_cast<std::size_t>(op)));
}

struct LegalCase {
    VideoState from;
    Op op;
    VideoState to;
};

void PrintTo(const LegalCase& c, std::ostream* os) {
    *os << name_of(c.from, c.op);
}

class LegalTransition : public testing::TestWithParam<LegalCase> {};

TEST_P(LegalTransition, EntersTheTargetStateAndBumpsTheVersionOnce) {
    const LegalCase& c = GetParam();
    Video video = video_in(c.from);
    ASSERT_TRUE(apply(video, c.op).has_value());
    EXPECT_EQ(video.state(), c.to);
    EXPECT_EQ(video.version(), kVersion + 1);
    EXPECT_EQ(video.id(), video_id());
    EXPECT_EQ(video.title(), kTitle);
    EXPECT_EQ(video.duration(),
              c.to == VideoState::Ready ? std::optional(kNewDuration) : std::nullopt);
    EXPECT_EQ(video.error_reason(),
              c.to == VideoState::Failed ? std::optional(kNewReason) : std::nullopt);
}

INSTANTIATE_TEST_SUITE_P(
    Video, LegalTransition,
    testing::Values(LegalCase{VideoState::Init, Op::StartProcessing, VideoState::Processing},
                    LegalCase{VideoState::Init, Op::MarkFailed, VideoState::Failed},
                    LegalCase{VideoState::Uploading, Op::StartProcessing, VideoState::Processing},
                    LegalCase{VideoState::Uploading, Op::MarkFailed, VideoState::Failed},
                    LegalCase{VideoState::Processing, Op::MarkReady, VideoState::Ready},
                    LegalCase{VideoState::Processing, Op::MarkFailed, VideoState::Failed}),
    [](const testing::TestParamInfo<LegalCase>& p) { return name_of(p.param.from, p.param.op); });

struct IllegalCase {
    VideoState from;
    Op op;
    DomainError error;
};

void PrintTo(const IllegalCase& c, std::ostream* os) {
    *os << name_of(c.from, c.op) << " -> " << core::to_string(c.error);
}

class IllegalTransition : public testing::TestWithParam<IllegalCase> {};

TEST_P(IllegalTransition, IsRejectedAndLeavesTheVideoUnchanged) {
    const IllegalCase& c = GetParam();
    Video video = video_in(c.from);
    const auto before = observe(video);
    EXPECT_EQ(apply(video, c.op), std::unexpected(c.error));
    EXPECT_EQ(observe(video), before);
}

INSTANTIATE_TEST_SUITE_P(
    Video, IllegalTransition,
    testing::Values(
        IllegalCase{VideoState::Init, Op::MarkReady, DomainError::InvalidTransition},
        IllegalCase{VideoState::Uploading, Op::MarkReady, DomainError::InvalidTransition},
        IllegalCase{VideoState::Processing, Op::StartProcessing, DomainError::InvalidTransition},
        IllegalCase{VideoState::Ready, Op::StartProcessing, DomainError::AlreadyTerminal},
        IllegalCase{VideoState::Ready, Op::MarkReady, DomainError::AlreadyTerminal},
        IllegalCase{VideoState::Ready, Op::MarkFailed, DomainError::AlreadyTerminal},
        IllegalCase{VideoState::Failed, Op::StartProcessing, DomainError::AlreadyTerminal},
        IllegalCase{VideoState::Failed, Op::MarkReady, DomainError::AlreadyTerminal},
        IllegalCase{VideoState::Failed, Op::MarkFailed, DomainError::AlreadyTerminal}),
    [](const testing::TestParamInfo<IllegalCase>& p) { return name_of(p.param.from, p.param.op); });

TEST(Video, CreateStartsInInitAtVersionZero) {
    const auto video = Video::create(video_id(), owner(), std::string(kTitle));
    ASSERT_TRUE(video.has_value());
    EXPECT_EQ(video->id(), video_id());
    EXPECT_EQ(video->owner(), owner());
    EXPECT_EQ(video->title(), kTitle);
    EXPECT_EQ(video->state(), VideoState::Init);
    EXPECT_EQ(video->version(), 0U);
    EXPECT_EQ(video->error_reason(), std::nullopt);
    EXPECT_EQ(video->duration(), std::nullopt);
}

TEST(Video, UploadedVideoWalksToReadyInTwoVersions) {
    auto video = Video::create(video_id(), owner(), std::string(kTitle));
    ASSERT_TRUE(video.has_value());
    ASSERT_TRUE(video->start_processing().has_value());
    ASSERT_TRUE(video->mark_ready(kDuration).has_value());
    EXPECT_EQ(video->state(), VideoState::Ready);
    EXPECT_EQ(video->version(), 2U);
    EXPECT_EQ(video->duration(), kDuration);
}

TEST(Video, KeepsTitlesByteForByte) {
    for (const std::string_view title : {
             std::string_view("  padded  "),
             std::string_view("Kyoto \xe4\xba\xac\xe9\x83\xbd, caf\xc3\xa9, \xf0\x90\x8d\x88"),
             std::string_view("a"),
         }) {
        const auto video = Video::create(video_id(), owner(), std::string(title));
        ASSERT_TRUE(video.has_value()) << title;
        EXPECT_EQ(video->title(), title);
    }
}

TEST(Video, TitleLimitCountsBytesNotCharacters) {
    std::string cyrillic;
    for (std::size_t i = 0; i < Video::kMaxTitleBytes / 2; ++i) {
        cyrillic += "\xd0\x96";
    }
    EXPECT_TRUE(Video::create(video_id(), owner(), cyrillic).has_value());
    EXPECT_TRUE(
        Video::create(video_id(), owner(), std::string(Video::kMaxTitleBytes, 't')).has_value());
    EXPECT_EQ(Video::create(video_id(), owner(), std::string(Video::kMaxTitleBytes + 1, 't')),
              std::unexpected(DomainError::InvalidTitle));
    EXPECT_EQ(Video::create(video_id(), owner(),
                            std::string(Video::kMaxTitleBytes - 1, 't') + "\xc3\xa9"),
              std::unexpected(DomainError::InvalidTitle));
}

TEST(Video, RejectsTitlesWithControlCharactersOrMalformedUtf8) {
    for (const std::string_view title : {
             std::string_view(""),
             std::string_view("tab\there"),
             std::string_view("line\nbreak"),
             std::string_view("del\x7f"),
             std::string_view("nul\0byte", 8),
             std::string_view("\x80"),
             std::string_view("truncated \xc3"),
             std::string_view("overlong \xc0\xaf"),
             std::string_view("overlong \xe0\x80\xaf"),
             std::string_view("surrogate \xed\xa0\x80"),
             std::string_view("beyond U+10FFFF \xf4\x90\x80\x80"),
             std::string_view("bad continuation \xe4\xba\x41"),
             std::string_view("\xff"),
         }) {
        EXPECT_EQ(Video::create(video_id(), owner(), std::string(title)),
                  std::unexpected(DomainError::InvalidTitle))
            << title;
    }
}

TEST(Video, MarkReadyRejectsANegativeDurationWithoutSideEffects) {
    Video video = video_in(VideoState::Processing);
    const auto before = observe(video);
    EXPECT_EQ(video.mark_ready(Millis{-1}), std::unexpected(DomainError::InvalidDuration));
    EXPECT_EQ(observe(video), before);
    EXPECT_TRUE(video.mark_ready(Millis{0}).has_value());
}

TEST(Video, MarkFailedRejectsAnEmptyReasonWithoutSideEffects) {
    Video video = video_in(VideoState::Uploading);
    const auto before = observe(video);
    EXPECT_EQ(video.mark_failed(""), std::unexpected(DomainError::MissingFailureReason));
    EXPECT_EQ(observe(video), before);
}

TEST(Video, MarkFailedRejectsMalformedReasonsWithoutSideEffects) {
    for (const std::string& reason :
         {std::string("worker lost\n"), std::string("exit \x01"), std::string("truncated \xc3"),
          std::string(Video::kMaxFailureReasonBytes + 1, 'r')}) {
        Video video = video_in(VideoState::Processing);
        const auto before = observe(video);
        EXPECT_EQ(video.mark_failed(reason), std::unexpected(DomainError::InvalidFailureReason))
            << reason.size();
        EXPECT_EQ(observe(video), before);
    }
}

TEST(Video, FailureReasonLimitIsInclusive) {
    Video video = video_in(VideoState::Processing);
    const std::string longest(Video::kMaxFailureReasonBytes, 'r');
    ASSERT_TRUE(video.mark_failed(longest).has_value());
    EXPECT_EQ(video.error_reason(), longest);
}

TEST(Video, StateIsJudgedBeforeArguments) {
    Video ready = video_in(VideoState::Ready);
    EXPECT_EQ(ready.mark_ready(Millis{-1}), std::unexpected(DomainError::AlreadyTerminal));
    Video failed = video_in(VideoState::Failed);
    EXPECT_EQ(failed.mark_failed(""), std::unexpected(DomainError::AlreadyTerminal));
    Video init = video_in(VideoState::Init);
    EXPECT_EQ(init.mark_ready(Millis{-1}), std::unexpected(DomainError::InvalidTransition));
}

TEST(Video, RehydrateRestoresEveryField) {
    VideoRecord record = record_in(VideoState::Ready);
    record.version = 41;
    const auto video = Video::rehydrate(record);
    ASSERT_TRUE(video.has_value());
    EXPECT_EQ(video->id(), record.id);
    EXPECT_EQ(video->owner(), record.owner);
    EXPECT_EQ(video->title(), record.title);
    EXPECT_EQ(video->state(), VideoState::Ready);
    EXPECT_EQ(video->version(), 41U);
    EXPECT_EQ(video->duration(), kDuration);
    EXPECT_EQ(video->error_reason(), std::nullopt);
}

TEST(Video, RehydrateAcceptsEveryConsistentState) {
    for (const VideoState state : {VideoState::Init, VideoState::Uploading, VideoState::Processing,
                                   VideoState::Ready, VideoState::Failed}) {
        const auto video = Video::rehydrate(record_in(state));
        ASSERT_TRUE(video.has_value()) << std::to_underlying(state);
        EXPECT_EQ(video->state(), state);
    }
}

struct CorruptCase {
    std::string_view name;
    VideoRecord record;
    DomainError error;
};

VideoRecord with_title(VideoRecord record, std::string title) {
    record.title = std::move(title);
    return record;
}

VideoRecord with_reason(VideoRecord record, std::optional<std::string> reason) {
    record.error_reason = std::move(reason);
    return record;
}

VideoRecord with_duration(VideoRecord record, std::optional<Millis> duration) {
    record.duration = duration;
    return record;
}

VideoRecord with_version(VideoRecord record, std::uint64_t version) {
    record.version = version;
    return record;
}

TEST(Video, RehydrateAcceptsTheLowestReachableVersions) {
    for (const auto& [state, version] :
         {std::pair{VideoState::Init, 0U}, std::pair{VideoState::Uploading, 1U},
          std::pair{VideoState::Processing, 1U}, std::pair{VideoState::Failed, 1U},
          std::pair{VideoState::Ready, 2U}}) {
        EXPECT_TRUE(Video::rehydrate(with_version(record_in(state), version)).has_value())
            << std::to_underlying(state);
    }
    const std::uint64_t highest = std::numeric_limits<std::uint64_t>::max() - 1;
    EXPECT_TRUE(Video::rehydrate(with_version(record_in(VideoState::Init), highest)).has_value());
}

TEST(Video, RehydrateRejectsRecordsThatContradictTheirState) {
    const std::array cases{
        CorruptCase{.name = "failed without reason",
                    .record = with_reason(record_in(VideoState::Failed), std::nullopt),
                    .error = DomainError::MissingFailureReason},
        CorruptCase{.name = "failed with empty reason",
                    .record = with_reason(record_in(VideoState::Failed), ""),
                    .error = DomainError::MissingFailureReason},
        CorruptCase{.name = "failed with a control character in the reason",
                    .record = with_reason(record_in(VideoState::Failed), "decoder\tcrashed"),
                    .error = DomainError::InvalidFailureReason},
        CorruptCase{.name = "failed with an oversized reason",
                    .record = with_reason(record_in(VideoState::Failed),
                                          std::string(Video::kMaxFailureReasonBytes + 1, 'r')),
                    .error = DomainError::InvalidFailureReason},
        CorruptCase{.name = "ready without duration",
                    .record = with_duration(record_in(VideoState::Ready), std::nullopt),
                    .error = DomainError::CorruptRecord},
        CorruptCase{.name = "ready with negative duration",
                    .record = with_duration(record_in(VideoState::Ready), Millis{-1}),
                    .error = DomainError::InvalidDuration},
        CorruptCase{.name = "processing with reason",
                    .record = with_reason(record_in(VideoState::Processing), std::string(kReason)),
                    .error = DomainError::CorruptRecord},
        CorruptCase{.name = "init with duration",
                    .record = with_duration(record_in(VideoState::Init), kDuration),
                    .error = DomainError::CorruptRecord},
        CorruptCase{.name = "failed with duration",
                    .record = with_duration(record_in(VideoState::Failed), kDuration),
                    .error = DomainError::CorruptRecord},
        CorruptCase{.name = "uploading at version 0",
                    .record = with_version(record_in(VideoState::Uploading), 0),
                    .error = DomainError::CorruptRecord},
        CorruptCase{.name = "processing at version 0",
                    .record = with_version(record_in(VideoState::Processing), 0),
                    .error = DomainError::CorruptRecord},
        CorruptCase{.name = "failed at version 0",
                    .record = with_version(record_in(VideoState::Failed), 0),
                    .error = DomainError::CorruptRecord},
        CorruptCase{.name = "ready at version 1",
                    .record = with_version(record_in(VideoState::Ready), 1),
                    .error = DomainError::CorruptRecord},
        CorruptCase{.name = "version that cannot be bumped",
                    .record = with_version(record_in(VideoState::Init),
                                           std::numeric_limits<std::uint64_t>::max()),
                    .error = DomainError::CorruptRecord},
        CorruptCase{.name = "empty title",
                    .record = with_title(record_in(VideoState::Init), ""),
                    .error = DomainError::InvalidTitle},
        CorruptCase{.name = "control character in title",
                    .record = with_title(record_in(VideoState::Ready), "a\rb"),
                    .error = DomainError::InvalidTitle},
    };
    for (const CorruptCase& c : cases) {
        EXPECT_EQ(Video::rehydrate(c.record), std::unexpected(c.error)) << c.name;
    }
}

} // namespace
