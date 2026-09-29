// S3Transfer's streams: their part size, and against a scripted S3 peer, a completion whose
// answer was lost.
#include "infra/s3util/credentials.hpp"
#include "infra/storage/s3_transfer.hpp"

#include "support/fake_clock.hpp"
#include "support/fake_random.hpp"
#include "support/http_test_server.hpp"

#include <cstdint>
#include <deque>
#include <gtest/gtest.h>
#include <mutex>
#include <span>
#include <string>
#include <vector>

namespace {

using infra::storage::S3Transfer;

constexpr std::uint64_t kMiB = std::uint64_t{1} << 20U;

TEST(StreamPartBytes, ASmallStreamGoesUpInSixteenMebibyteParts) {
    EXPECT_EQ(S3Transfer::stream_part_bytes(0), 16 * kMiB);
    EXPECT_EQ(S3Transfer::stream_part_bytes(kMiB * 16 * 9'000), 16 * kMiB);
}

TEST(StreamPartBytes, ALargerBoundFitsInNineThousandPartsOfWholeMebibytes) {
    // One byte past 9,000 parts of 16 MiB needs the next whole MiB.
    EXPECT_EQ(S3Transfer::stream_part_bytes((kMiB * 16 * 9'000) + 1), 17 * kMiB);
    // 12 h at 100 Mbit/s, the packager's ceiling, with an eighth more for the container.
    const std::uint64_t longest = std::uint64_t{100'000} * 125 * 43'200 * 9 / 8;
    const std::uint64_t part = S3Transfer::stream_part_bytes(longest);
    EXPECT_EQ(part % kMiB, 0U);
    EXPECT_GE(part * 9'000, longest);
    EXPECT_LT((part - kMiB) * 9'000, longest);
    EXPECT_EQ(part, 65 * kMiB);
}

using ulw::test::Reply;
using ulw::test::ServedRequest;

Reply reply(int status, std::string body,
            std::vector<std::pair<std::string, std::string>> headers = {}) {
    return Reply{.status = status, .headers = std::move(headers), .body = std::move(body)};
}

class S3StreamTest : public ::testing::Test {
protected:
    S3StreamTest()
        : server([this](const ServedRequest& /*r*/) {
              const std::scoped_lock lock(mutex);
              if (script.empty()) {
                  return reply(500, "<Error><Code>InternalError</Code></Error>");
              }
              Reply next = std::move(script.front());
              script.pop_front();
              return next;
          }) {}

    std::unique_ptr<S3Transfer> transfer() {
        auto created =
            S3Transfer::create({.credentials = credentials,
                                .clock = clock,
                                .random = random,
                                .profile = *infra::s3util::S3Profile::minio(server.base_url()),
                                .bucket = "media"},
                               {.base = core::Millis{1}, .cap = core::Millis{2}, .max_retries = 3});
        EXPECT_TRUE(created);
        return created ? std::move(*created) : nullptr;
    }

    void then(std::vector<Reply> replies) {
        const std::scoped_lock lock(mutex);
        for (auto& r : replies) {
            script.push_back(std::move(r));
        }
    }

    ulw::test::FakeClock clock;
    ulw::test::FakeRandom random;
    infra::s3util::StaticCredentialProvider credentials{
        *infra::s3util::Credentials::make("AKIDEXAMPLE", infra::s3util::SecretString("secret"))};
    std::mutex mutex;
    std::deque<Reply> script;
    ulw::test::HttpTestServer server;
};

const std::string kInitiate = "<InitiateMultipartUploadResult><Bucket>media</Bucket><Key>k</Key>"
                              "<UploadId>u1</UploadId></InitiateMultipartUploadResult>";

TEST_F(S3StreamTest, ACompletionWhoseAnswerWasLostIsTakenFromTheObjectItLeft) {
    const auto store = transfer();
    ASSERT_NE(store, nullptr);
    const auto key = *core::StorageKey::parse("videos/v/raw");
    const std::string bytes(1000, 'x');
    // The completion's retry finds the upload gone: the first attempt completed it.
    then({reply(200, kInitiate), reply(200, "", {{"etag", "\"e1\""}}),
          reply(500, "<Error><Code>InternalError</Code></Error>"),
          reply(404, "<Error><Code>NoSuchUpload</Code></Error>"),
          reply(200, "", {{"content-length", "1000"}})});
    auto stream = store->begin(key, *core::ContentType::parse("video/mp2t"), 1U << 20U);
    ASSERT_TRUE(stream);
    ASSERT_TRUE((*stream)->write(std::as_bytes(std::span(bytes))));
    EXPECT_TRUE((*stream)->commit());
}

TEST_F(S3StreamTest, AnUploadGoneWithoutItsObjectIsNotTakenForCommitted) {
    const auto store = transfer();
    ASSERT_NE(store, nullptr);
    const auto key = *core::StorageKey::parse("videos/v/raw");
    const std::string bytes(1000, 'x');
    then({reply(200, kInitiate), reply(200, "", {{"etag", "\"e1\""}}),
          reply(404, "<Error><Code>NoSuchUpload</Code></Error>"),
          reply(404, "", {{"content-length", "0"}})});
    auto stream = store->begin(key, *core::ContentType::parse("video/mp2t"), 1U << 20U);
    ASSERT_TRUE(stream);
    ASSERT_TRUE((*stream)->write(std::as_bytes(std::span(bytes))));
    EXPECT_EQ((*stream)->commit().error(), core::ports::StorageError::NotFound);
}

} // namespace
