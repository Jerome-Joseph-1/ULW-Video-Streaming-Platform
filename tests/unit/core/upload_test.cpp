#include "core/errors/domain_error.hpp"
#include "core/models/ids.hpp"
#include "core/models/upload.hpp"
#include "core/util/time.hpp"

#include "support/fake_clock.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <gtest/gtest.h>
#include <ostream>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

namespace {

using core::DomainError;
using core::Millis;
using core::Upload;
using core::UploadId;
using core::UploadRecord;
using core::UploadState;
using core::UserId;
using core::VideoId;

constexpr std::uint64_t kMiB = 1ULL << 20U;
constexpr std::uint64_t kChunk = 8 * kMiB;
// Deliberately not a multiple of the chunk size: the last chunk is short.
constexpr std::uint64_t kSize = (2 * kChunk) + 3;

UploadId upload_id() {
    return UploadId::parse("0192f3c4-7a1b-7c2d-8e3f-0123456789ab").value();
}

VideoId video_id() {
    return VideoId::parse("0192f3c4-7a1b-7c2d-8e3f-ba9876543210").value();
}

UserId owner() {
    return UserId::parse("auth0|5f7c8ec7c33c6c004bbafe82").value();
}

core::WallTime expiry() {
    return ulw::test::FakeClock{}.wall_now() + std::chrono::hours{24};
}

UploadRecord record_in(UploadState state, std::uint64_t durable_offset) {
    return UploadRecord{.id = upload_id(),
                        .video_id = video_id(),
                        .owner = owner(),
                        .size_bytes = kSize,
                        .chunk_size = kChunk,
                        .durable_offset = durable_offset,
                        .state = state,
                        .expires_at = expiry()};
}

Upload upload_in(UploadState state, std::uint64_t durable_offset) {
    return Upload::rehydrate(record_in(state, durable_offset)).value();
}

auto observe(const Upload& upload) {
    return std::tuple{upload.id().to_string(),
                      upload.video_id().to_string(),
                      std::string(upload.owner().view()),
                      upload.size_bytes(),
                      upload.chunk_size(),
                      upload.durable_offset(),
                      upload.state(),
                      upload.expires_at().time_since_epoch().count()};
}

TEST(Upload, CreateStartsActiveAtOffsetZero) {
    const auto upload = Upload::create(upload_id(), video_id(), owner(), kSize, kChunk, expiry());
    ASSERT_TRUE(upload.has_value());
    EXPECT_EQ(upload->id(), upload_id());
    EXPECT_EQ(upload->video_id(), video_id());
    EXPECT_EQ(upload->owner(), owner());
    EXPECT_EQ(upload->size_bytes(), kSize);
    EXPECT_EQ(upload->chunk_size(), kChunk);
    EXPECT_EQ(upload->durable_offset(), 0U);
    EXPECT_EQ(upload->state(), UploadState::Active);
    EXPECT_EQ(upload->expires_at(), expiry());
}

TEST(Upload, CreateEnforcesTheSizeBoundsExactly) {
    const auto create = [](std::uint64_t size_bytes, std::uint64_t chunk_size) {
        return Upload::create(upload_id(), video_id(), owner(), size_bytes, chunk_size, expiry());
    };
    EXPECT_TRUE(create(1, kChunk).has_value());
    EXPECT_TRUE(create(Upload::kMaxSizeBytes, kChunk).has_value());
    EXPECT_EQ(create(0, kChunk), std::unexpected(DomainError::InvalidUploadSize));
    EXPECT_EQ(create(Upload::kMaxSizeBytes + 1, kChunk),
              std::unexpected(DomainError::InvalidUploadSize));
    EXPECT_EQ(create(kSize, 0), std::unexpected(DomainError::InvalidChunkSize));
}

TEST(Upload, AdvanceMovesTheDurableOffsetUpToTheSize) {
    Upload upload = upload_in(UploadState::Active, 0);
    ASSERT_TRUE(upload.advance_to(kChunk).has_value());
    EXPECT_EQ(upload.durable_offset(), kChunk);
    ASSERT_TRUE(upload.advance_to(kSize).has_value());
    EXPECT_EQ(upload.durable_offset(), kSize);
    EXPECT_EQ(upload.state(), UploadState::Active);
}

TEST(Upload, AdvanceToTheCurrentOffsetIsAcceptedWithoutEffect) {
    Upload upload = upload_in(UploadState::Active, kChunk);
    const auto before = observe(upload);
    EXPECT_TRUE(upload.advance_to(kChunk).has_value());
    EXPECT_EQ(observe(upload), before);
}

TEST(Upload, AdvanceRejectsARegressionWithoutSideEffects) {
    Upload upload = upload_in(UploadState::Active, kChunk);
    const auto before = observe(upload);
    EXPECT_EQ(upload.advance_to(kChunk - 1), std::unexpected(DomainError::OffsetRegression));
    EXPECT_EQ(observe(upload), before);
}

TEST(Upload, AdvanceRejectsAnOffsetPastTheSizeWithoutSideEffects) {
    Upload upload = upload_in(UploadState::Active, kChunk);
    const auto before = observe(upload);
    EXPECT_EQ(upload.advance_to(kSize + 1), std::unexpected(DomainError::OffsetBeyondSize));
    EXPECT_EQ(observe(upload), before);
}

TEST(Upload, CompleteRequiresEveryByteToBeDurable) {
    Upload upload = upload_in(UploadState::Active, kSize - 1);
    const auto before = observe(upload);
    EXPECT_EQ(upload.complete(), std::unexpected(DomainError::UploadIncomplete));
    EXPECT_EQ(observe(upload), before);

    ASSERT_TRUE(upload.advance_to(kSize).has_value());
    ASSERT_TRUE(upload.complete().has_value());
    EXPECT_EQ(upload.state(), UploadState::Completed);
    EXPECT_EQ(upload.durable_offset(), kSize);
}

TEST(Upload, AbortEndsAnActiveUploadAndKeepsItsOffset) {
    Upload upload = upload_in(UploadState::Active, kChunk);
    ASSERT_TRUE(upload.abort().has_value());
    EXPECT_EQ(upload.state(), UploadState::Aborted);
    EXPECT_EQ(upload.durable_offset(), kChunk);
}

enum class Op : std::uint8_t { AdvanceTo, Complete, Abort };

std::expected<void, DomainError> apply(Upload& upload, Op op) {
    switch (op) {
    case Op::AdvanceTo:
        return upload.advance_to(upload.durable_offset());
    case Op::Complete:
        return upload.complete();
    case Op::Abort:
        return upload.abort();
    }
    std::unreachable();
}

struct FinishedCase {
    UploadState state;
    Op op;
};

std::string name_of(const FinishedCase& c) {
    constexpr std::array<std::string_view, 3> kStates{"Active", "Completed", "Aborted"};
    constexpr std::array<std::string_view, 3> kOps{"AdvanceTo", "Complete", "Abort"};
    return std::string(kStates.at(static_cast<std::size_t>(c.state))) + "_" +
           std::string(kOps.at(static_cast<std::size_t>(c.op)));
}

void PrintTo(const FinishedCase& c, std::ostream* os) {
    *os << name_of(c);
}

class FinishedUpload : public testing::TestWithParam<FinishedCase> {};

TEST_P(FinishedUpload, RejectsEveryOperationWithoutSideEffects) {
    const FinishedCase& c = GetParam();
    // At full size every operation would succeed on an active upload, so only the state can
    // explain a rejection.
    Upload upload = upload_in(c.state, kSize);
    const auto before = observe(upload);
    EXPECT_EQ(apply(upload, c.op), std::unexpected(DomainError::UploadNotActive));
    EXPECT_EQ(observe(upload), before);
}

INSTANTIATE_TEST_SUITE_P(Upload, FinishedUpload,
                         testing::Values(FinishedCase{UploadState::Completed, Op::AdvanceTo},
                                         FinishedCase{UploadState::Completed, Op::Complete},
                                         FinishedCase{UploadState::Completed, Op::Abort},
                                         FinishedCase{UploadState::Aborted, Op::AdvanceTo},
                                         FinishedCase{UploadState::Aborted, Op::Complete},
                                         FinishedCase{UploadState::Aborted, Op::Abort}),
                         [](const testing::TestParamInfo<FinishedCase>& p) {
                             return name_of(p.param);
                         });

TEST(Upload, ExpiresExactlyAtTheDeadline) {
    const Upload upload = upload_in(UploadState::Active, 0);
    EXPECT_FALSE(upload.is_expired(expiry() - Millis{1}));
    EXPECT_TRUE(upload.is_expired(expiry()));
    EXPECT_TRUE(upload.is_expired(expiry() + Millis{1}));
}

TEST(Upload, IsOwnedOnlyByItsOwner) {
    const Upload upload = upload_in(UploadState::Active, 0);
    EXPECT_TRUE(upload.owned_by(owner()));
    EXPECT_FALSE(upload.owned_by(UserId::parse("auth0|5f7c8ec7c33c6c004bbafe83").value()));
}

TEST(Upload, RehydrateRestoresEveryField) {
    const UploadRecord record = record_in(UploadState::Aborted, kChunk + 5);
    const auto upload = Upload::rehydrate(record);
    ASSERT_TRUE(upload.has_value());
    EXPECT_EQ(upload->id(), record.id);
    EXPECT_EQ(upload->video_id(), record.video_id);
    EXPECT_EQ(upload->owner(), record.owner);
    EXPECT_EQ(upload->size_bytes(), kSize);
    EXPECT_EQ(upload->chunk_size(), kChunk);
    EXPECT_EQ(upload->durable_offset(), kChunk + 5);
    EXPECT_EQ(upload->state(), UploadState::Aborted);
    EXPECT_EQ(upload->expires_at(), record.expires_at);
}

TEST(Upload, RehydrateRejectsInconsistentRecords) {
    const UploadRecord beyond = record_in(UploadState::Active, kSize + 1);
    EXPECT_EQ(Upload::rehydrate(beyond), std::unexpected(DomainError::OffsetBeyondSize));

    const UploadRecord short_completed = record_in(UploadState::Completed, kSize - 1);
    EXPECT_EQ(Upload::rehydrate(short_completed), std::unexpected(DomainError::CorruptRecord));

    UploadRecord empty = record_in(UploadState::Active, 0);
    empty.size_bytes = 0;
    EXPECT_EQ(Upload::rehydrate(empty), std::unexpected(DomainError::InvalidUploadSize));

    UploadRecord oversized = record_in(UploadState::Active, 0);
    oversized.size_bytes = Upload::kMaxSizeBytes + 1;
    EXPECT_EQ(Upload::rehydrate(oversized), std::unexpected(DomainError::InvalidUploadSize));

    UploadRecord no_chunk = record_in(UploadState::Active, 0);
    no_chunk.chunk_size = 0;
    EXPECT_EQ(Upload::rehydrate(no_chunk), std::unexpected(DomainError::InvalidChunkSize));
}

} // namespace
