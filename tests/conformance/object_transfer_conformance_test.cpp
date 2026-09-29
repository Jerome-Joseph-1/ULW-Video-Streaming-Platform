// Laws every IObjectTransfer backend obeys, run against the filesystem always and against
// live buckets under ULW_CONFORMANCE_LIVE.
#include "core/ports/object_transfer.hpp"
#include "infra/storage/fs_transfer.hpp"

#include "support/temp_dir.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <gtest/gtest.h>
#include <iterator>
#include <memory>
#include <ostream>
#include <span>
#include <string>
#include <vector>

#ifdef ULW_CONFORMANCE_LIVE
#include "infra/curl/http.hpp"
#include "infra/s3util/url.hpp"
#include "infra/storage/s3_transfer.hpp"
#include "os/system_clock.hpp"
#include "os/system_random.hpp"

#include "support/live_s3.hpp"
#endif

namespace {

namespace fs = std::filesystem;
using core::ports::StorageError;
using ulw::test::TempDir;

constexpr std::size_t kMiB = std::size_t{1} << 20U;

std::vector<std::byte> pattern(std::size_t n, std::size_t seed = 0) {
    std::vector<std::byte> v(n);
    for (std::size_t i = 0; i < n; ++i) {
        v[i] = static_cast<std::byte>((((i + seed) * 31) + 7) % 251);
    }
    return v;
}

core::StorageKey key_of(const std::string& text) {
    return *core::StorageKey::parse(text);
}

const core::ContentType& segment_type() {
    static const auto type = *core::ContentType::parse("video/iso.segment");
    return type;
}

void write_file(const fs::path& p, std::span<const std::byte> bytes) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    for (const std::byte b : bytes) {
        out.put(static_cast<char>(b));
    }
}

std::vector<std::byte> read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::vector<std::byte> out;
    for (auto it = std::istreambuf_iterator<char>(in); it != std::istreambuf_iterator<char>();
         ++it) {
        out.push_back(static_cast<std::byte>(*it));
    }
    return out;
}

class TransferHarness {
public:
    TransferHarness() = default;
    TransferHarness(const TransferHarness&) = delete;
    TransferHarness& operator=(const TransferHarness&) = delete;
    virtual ~TransferHarness() = default;

    virtual core::ports::IObjectTransfer& transfer() = 0;
    // A key no other run uses; removed again when the harness goes.
    virtual core::StorageKey key(const std::string& name) = 0;
};

class FsHarness final : public TransferHarness {
public:
    FsHarness() : transfer_(root_.path()) {}
    core::ports::IObjectTransfer& transfer() override { return transfer_; }
    core::StorageKey key(const std::string& name) override { return key_of("videos/" + name); }

private:
    TempDir root_{"ulw-transfer"};
    infra::storage::FsTransfer transfer_;
};

#ifdef ULW_CONFORMANCE_LIVE
class S3Harness final : public TransferHarness {
public:
    explicit S3Harness(ulw::test::LiveS3 target)
        : target_(std::move(target)), prefix_(ulw::test::unique_prefix("transfer")) {
        transfer_ =
            std::move(*infra::storage::S3Transfer::create({.credentials = target_.credentials,
                                                           .clock = clock_,
                                                           .random = random_,
                                                           .profile = target_.profile,
                                                           .bucket = target_.bucket}));
    }
    ~S3Harness() override {
        const auto bucket = *infra::s3util::Bucket::make(target_.profile, target_.bucket);
        for (const auto& k : keys_) {
            [[maybe_unused]] const auto removed =
                ulw::test::send(target_, infra::curl::Method::Delete, bucket.object(k));
        }
    }
    S3Harness(const S3Harness&) = delete;
    S3Harness& operator=(const S3Harness&) = delete;

    core::ports::IObjectTransfer& transfer() override { return *transfer_; }
    core::StorageKey key(const std::string& name) override {
        keys_.push_back(key_of(prefix_ + name));
        return keys_.back();
    }

private:
    ulw::test::LiveS3 target_;
    std::string prefix_;
    os::SystemClock clock_;
    os::SystemRandom random_;
    std::unique_ptr<infra::storage::S3Transfer> transfer_;
    std::vector<core::StorageKey> keys_;
};

std::unique_ptr<TransferHarness> live_harness(ulw::test::LiveS3 target) {
    if (!ulw::test::ensure_bucket(target)) {
        return nullptr;
    }
    return std::make_unique<S3Harness>(std::move(target));
}
#endif

struct Backend {
    std::string name;
    std::function<std::unique_ptr<TransferHarness>()> make;
    friend void PrintTo(const Backend& b, std::ostream* os) { *os << b.name; }
};

std::vector<Backend> backends() {
    std::vector<Backend> out;
    out.push_back({"fs", [] { return std::make_unique<FsHarness>(); }});
#ifdef ULW_CONFORMANCE_LIVE
    out.push_back({"minio", [] { return live_harness(ulw::test::minio_from_env()); }});
    if (auto r2 = ulw::test::r2_from_env()) {
        out.push_back({"r2", [r2 = *std::move(r2)] { return live_harness(r2); }});
    }
#endif
    return out;
}

class TransferLaws : public ::testing::TestWithParam<Backend> {
protected:
    void SetUp() override {
        harness_ = GetParam().make();
        ASSERT_NE(harness_, nullptr) << GetParam().name << " unreachable";
    }

    core::ports::IObjectTransfer& transfer() { return harness_->transfer(); }
    core::StorageKey key(const std::string& name) { return harness_->key(name); }
    [[nodiscard]] fs::path local(const std::string& name) const { return files_.path() / name; }

private:
    std::unique_ptr<TransferHarness> harness_;
    TempDir files_{"ulw-transfer-files"};
};

TEST_P(TransferLaws, AnUploadedFileDownloadsByteForByteAndReportsItsSize) {
    // Larger than any buffer on the way, so every one of them wraps at least once.
    const auto bytes = pattern((6 * kMiB) + 123);
    write_file(local("up"), bytes);
    const auto k = key("hls/720p/seg_00000.m4s");
    ASSERT_TRUE(transfer().upload(local("up"), k, segment_type()));

    EXPECT_EQ(transfer().size(k), bytes.size());
    const auto downloaded = transfer().download(k, local("down"));
    ASSERT_TRUE(downloaded);
    EXPECT_EQ(*downloaded, bytes.size());
    EXPECT_TRUE(std::ranges::equal(read_file(local("down")), bytes));
}

TEST_P(TransferLaws, AnUploadReplacesTheObjectUnderTheSameKey) {
    const auto first = pattern(kMiB, 1);
    const auto second = pattern(1000, 2);
    write_file(local("first"), first);
    write_file(local("second"), second);
    const auto k = key("hls/master.m3u8");
    ASSERT_TRUE(transfer().upload(local("first"), k, segment_type()));
    ASSERT_TRUE(transfer().upload(local("second"), k, segment_type()));

    EXPECT_EQ(transfer().size(k), second.size());
    ASSERT_TRUE(transfer().download(k, local("down")));
    EXPECT_TRUE(std::ranges::equal(read_file(local("down")), second));
}

TEST_P(TransferLaws, AnUploadNewCreatesTheObjectWhenTheKeyIsFree) {
    const auto bytes = pattern(1000, 3);
    write_file(local("up"), bytes);
    const auto k = key("live/claim");
    ASSERT_TRUE(transfer().upload_new(local("up"), k, segment_type()));
    ASSERT_TRUE(transfer().download(k, local("down")));
    EXPECT_TRUE(std::ranges::equal(read_file(local("down")), bytes));
}

TEST_P(TransferLaws, AnUploadNewNeverReplacesAnExistingObject) {
    const auto first = pattern(1000, 4);
    const auto second = pattern(2000, 5);
    write_file(local("first"), first);
    write_file(local("second"), second);
    const auto k = key("live/claim");
    ASSERT_TRUE(transfer().upload_new(local("first"), k, segment_type()));
    EXPECT_EQ(transfer().upload_new(local("second"), k, segment_type()).error(),
              StorageError::AlreadyExists);
    ASSERT_TRUE(transfer().download(k, local("down")));
    EXPECT_TRUE(std::ranges::equal(read_file(local("down")), first));
}

TEST_P(TransferLaws, AnUploadNewOverAnObjectPutByUploadIsRefusedToo) {
    write_file(local("first"), pattern(10, 6));
    write_file(local("second"), pattern(20, 7));
    const auto k = key("live/claim");
    ASSERT_TRUE(transfer().upload(local("first"), k, segment_type()));
    EXPECT_EQ(transfer().upload_new(local("second"), k, segment_type()).error(),
              StorageError::AlreadyExists);
    EXPECT_EQ(transfer().size(k), 10U);
}

TEST_P(TransferLaws, ADownloadReplacesWhateverTheDestinationHeld) {
    const auto bytes = pattern(4096, 3);
    write_file(local("up"), bytes);
    write_file(local("down"), pattern(2 * kMiB, 4));
    const auto k = key("raw");
    ASSERT_TRUE(transfer().upload(local("up"), k, segment_type()));

    ASSERT_TRUE(transfer().download(k, local("down")));
    EXPECT_TRUE(std::ranges::equal(read_file(local("down")), bytes));
}

TEST_P(TransferLaws, AnEmptyObjectRoundTrips) {
    write_file(local("up"), {});
    const auto k = key("empty");
    ASSERT_TRUE(transfer().upload(local("up"), k, segment_type()));
    EXPECT_EQ(transfer().size(k), 0U);
    EXPECT_EQ(transfer().download(k, local("down")), 0U);
    EXPECT_TRUE(fs::exists(local("down")));
}

TEST_P(TransferLaws, AMissingObjectIsNotFound) {
    const auto k = key("never-written");
    EXPECT_EQ(transfer().size(k).error(), StorageError::NotFound);
    EXPECT_EQ(transfer().download(k, local("down")).error(), StorageError::NotFound);
}

TEST_P(TransferLaws, UploadingAMissingFileFailsAndCreatesNothing) {
    const auto k = key("from-nowhere");
    EXPECT_FALSE(transfer().upload(local("absent"), k, segment_type()));
    EXPECT_EQ(transfer().size(k).error(), StorageError::NotFound);
}

TEST_P(TransferLaws, AnUploadNeverFollowsASymbolicLink) {
    // What the worker uploads, a sandboxed ffmpeg wrote; a link there could name any file the
    // worker can read.
    write_file(local("private"), pattern(100, 7));
    fs::create_symlink(local("private"), local("link"));
    const auto k = key("through-a-link");
    EXPECT_FALSE(transfer().upload(local("link"), k, segment_type()));
    EXPECT_EQ(transfer().size(k).error(), StorageError::NotFound);
}

INSTANTIATE_TEST_SUITE_P(Backends, TransferLaws, ::testing::ValuesIn(backends()),
                         [](const auto& param_info) { return param_info.param.name; });

// The worker and the gateway share a root in development, so FsTransfer keeps to FsStore's
// layout for committed objects: the file objects/<key>, replaced by a rename from a sibling
// whose name ends in ".tmp", which FsStore::list() skips.
TEST(FsTransfer, KeepsCommittedObjectsWhereFsStoreDoes) {
    const TempDir root("ulw-fs-layout");
    const TempDir files("ulw-fs-layout-files");
    infra::storage::FsTransfer transfer(root.path());

    const auto committed = pattern(5000, 5);
    fs::create_directories(root.path() / "objects" / "videos" / "v");
    write_file(root.path() / "objects" / "videos" / "v" / "raw", committed);
    ASSERT_TRUE(transfer.download(key_of("videos/v/raw"), files.path() / "raw"));
    EXPECT_TRUE(std::ranges::equal(read_file(files.path() / "raw"), committed));

    const auto published = pattern(700, 6);
    write_file(files.path() / "index", published);
    ASSERT_TRUE(transfer.upload(files.path() / "index", key_of("videos/v/hls/360p/index.m3u8"),
                                segment_type()));
    const fs::path dir = root.path() / "objects" / "videos" / "v" / "hls" / "360p";
    EXPECT_TRUE(std::ranges::equal(read_file(dir / "index.m3u8"), published));
    EXPECT_EQ(std::distance(fs::directory_iterator(dir), fs::directory_iterator()), 1);
}

} // namespace
