#include "storage_harness.hpp"

#include "infra/storage/fs_store.hpp"
#include "net/offload_pool.hpp"
#include "net/reactor_factory.hpp"
#include "os/system_clock.hpp"
#include "os/system_random.hpp"

#include <cstdlib>
#include <filesystem>
#include <gtest/gtest.h>

#ifdef ULW_CONFORMANCE_LIVE
#include "infra/curl/multi.hpp"
#include "infra/storage/s3_store.hpp"

#include "support/live_s3.hpp"
#endif

namespace ulw::test {

namespace {

os::SystemClock& system_clock() {
    static os::SystemClock clock;
    return clock;
}

std::unique_ptr<net::IReactor> make_loop() {
    auto r = net::make_reactor(reactor_kind_from_env(), system_clock(), 4096);
    return r ? std::move(*r) : nullptr;
}

class FsHarness final : public StorageHarness {
public:
    FsHarness() {
        std::string tmpl = (std::filesystem::temp_directory_path() / "ulw-fs-XXXXXX").string();
        root_ = ::mkdtemp(tmpl.data());
        reactor_ = make_loop();
        // Two threads: enough for one upload's write to overlap another's.
        auto writer = std::move(*net::OffloadPool::create(*reactor_, 2));
        writer_ = writer.get();
        store_ = std::make_unique<infra::storage::FsStore>(
            infra::storage::FsStore::Deps{.clock = system_clock(), .random = random_},
            std::move(writer), root_, kLocalChunk);
    }
    ~FsHarness() override {
        store_.reset();
        std::error_code ec;
        std::filesystem::remove_all(root_, ec);
    }
    FsHarness(const FsHarness&) = delete;
    FsHarness& operator=(const FsHarness&) = delete;

    core::ports::IIngestStore& ingest() override { return *store_; }
    core::ports::IObjectReader& reader() override { return *store_; }
    core::ports::IObjectAdmin& admin() override { return *store_; }

    // Every write the store accepted is a pool job, and a job has finished once its
    // complete() has run on the loop.
    void settle(Observer& /*observer*/) override {
        if (!pump_until(*reactor_, [&] { return writer_->in_flight() == 0; }, kOperationLimit)) {
            ADD_FAILURE() << "offload pool never drained";
        }
    }

private:
    std::filesystem::path root_;
    os::SystemRandom random_;
    // The store's, borrowed to tell when every write it accepted has completed.
    net::OffloadPool* writer_ = nullptr;
    std::unique_ptr<infra::storage::FsStore> store_;
};

#ifdef ULW_CONFORMANCE_LIVE
// A live bucket through the S3 adapter. Parts are 5 MiB, the smallest S3, MinIO and R2 accept
// for any part but the last, so every law still spans several parts.
class S3Harness final : public StorageHarness {
public:
    static constexpr std::uint64_t kPartSize = std::uint64_t{5} << 20U;

    explicit S3Harness(LiveS3 target)
        : target_(std::move(target)), prefix_(unique_prefix("conformance")) {
        reactor_ = make_loop();
        multi_ = std::move(*infra::curl::Multi::create(*reactor_));
        store_ = std::move(*infra::storage::S3Store::create(
            infra::storage::S3Store::Deps{.reactor = *reactor_,
                                          .multi = *multi_,
                                          .credentials = target_.credentials,
                                          .clock = system_clock(),
                                          .random = random_,
                                          .profile = target_.profile,
                                          .bucket = target_.bucket},
            infra::storage::S3StoreOptions{.part_size = kPartSize}));
    }
    ~S3Harness() override {
        // What a law committed or left unfinished would otherwise pile up in a bucket that
        // outlives the run.
        if (auto keys = store_->list(prefix_)) {
            for (const auto& key : *keys) {
                [[maybe_unused]] const auto removed = store_->remove(key);
            }
        }
        abort_uploads(target_, prefix_);
        store_.reset();
        multi_.reset();
    }
    S3Harness(const S3Harness&) = delete;
    S3Harness& operator=(const S3Harness&) = delete;

    core::ports::IIngestStore& ingest() override { return *store_; }
    core::ports::IObjectReader& reader() override { return *store_; }
    core::ports::IObjectAdmin& admin() override { return *store_; }
    [[nodiscard]] std::string key_prefix() const override { return prefix_; }
    // A part is durable only once the store answers it, a network round trip away.
    [[nodiscard]] std::chrono::milliseconds quiet_period() const override {
        return std::chrono::seconds(2);
    }

private:
    LiveS3 target_;
    std::string prefix_;
    os::SystemRandom random_;
    std::unique_ptr<infra::curl::Multi> multi_;
    std::unique_ptr<infra::storage::S3Store> store_;
};

// Under ULW_CONFORMANCE_LIVE an unreachable backend is a failure, not a skip: the harness
// comes back null and the suite's SetUp asserts on it.
std::unique_ptr<StorageHarness> live_harness(LiveS3 target) {
    if (!ensure_bucket(target)) {
        return nullptr;
    }
    return std::make_unique<S3Harness>(std::move(target));
}
#endif

} // namespace

net::ReactorKind reactor_kind_from_env() {
    const char* env = std::getenv("ULW_REACTOR");
    if (env == nullptr) {
        return net::ReactorKind::IoUring;
    }
    return net::parse_reactor_kind(env).value_or(net::ReactorKind::IoUring);
}

bool StorageHarness::write_all(core::ports::IIngestSession& session, Observer& observer,
                               std::span<const std::byte> bytes) {
    while (!bytes.empty()) {
        const std::size_t n = session.write(bytes);
        bytes = bytes.subspan(n);
        if (n > 0) {
            continue;
        }
        if (session.state() != core::ports::IngestState::Open) {
            return false;
        }
        const int before = observer.calls;
        if (!pump_until(*reactor_, [&] { return observer.calls != before; }, kOperationLimit)) {
            return false;
        }
    }
    return true;
}

bool StorageHarness::finish(core::ports::IIngestSession& session, Observer& /*observer*/) {
    session.finish();
    const auto done = [&] {
        const auto s = session.state();
        return s == core::ports::IngestState::Committed || s == core::ports::IngestState::Failed;
    };
    return pump_until(*reactor_, done, kOperationLimit) &&
           session.state() == core::ports::IngestState::Committed;
}

void StorageHarness::settle(Observer& observer) {
    int seen = observer.calls;
    auto quiet_since = std::chrono::steady_clock::now();
    const auto limit = quiet_since + kOperationLimit;
    while (std::chrono::steady_clock::now() - quiet_since < quiet_period() &&
           std::chrono::steady_clock::now() < limit) {
        reactor_->run_once(core::Millis{5});
        if (observer.calls != seen) {
            seen = observer.calls;
            quiet_since = std::chrono::steady_clock::now();
        }
    }
}

core::ports::IngestId upload_whole(StorageHarness& h, const core::StorageKey& key,
                                   std::span<const std::byte> data) {
    auto id = h.ingest().create(key, data.size(), *core::ContentType::parse("video/mp4"));
    EXPECT_TRUE(id);
    Observer obs;
    auto session = h.ingest().open(*id, 0, obs);
    EXPECT_TRUE(session);
    EXPECT_TRUE(h.write_all(**session, obs, data));
    EXPECT_TRUE(h.finish(**session, obs));
    return *id;
}

FakeHarness::FakeHarness() {
    reactor_ = make_loop();
    store_ = std::make_unique<infra::storage::FakeStore>(*reactor_, system_clock(), kLocalChunk);
}

FakeHarness::~FakeHarness() = default;
core::ports::IIngestStore& FakeHarness::ingest() {
    return *store_;
}
core::ports::IObjectReader& FakeHarness::reader() {
    return *store_;
}
core::ports::IObjectAdmin& FakeHarness::admin() {
    return *store_;
}

// The fake does all its asynchronous work in zero-delay timers, and nothing else is on this
// loop, so a turn that dispatches nothing means no work is left.
void FakeHarness::settle(Observer& /*observer*/) {
    const auto limit = std::chrono::steady_clock::now() + kOperationLimit;
    while (reactor_->run_once(core::Millis{0}) != 0) {
        if (std::chrono::steady_clock::now() > limit) {
            ADD_FAILURE() << "fake store never went idle";
            return;
        }
    }
}

std::vector<StoreFactory> store_factories() {
    std::vector<StoreFactory> out;
    out.push_back({"fake", [] { return std::make_unique<FakeHarness>(); }});
    out.push_back({"fs", [] { return std::make_unique<FsHarness>(); }});
#ifdef ULW_CONFORMANCE_LIVE
    out.push_back({"minio", [] { return live_harness(minio_from_env()); }});
    if (auto r2 = r2_from_env()) {
        out.push_back({"r2", [r2 = *std::move(r2)] { return live_harness(r2); }});
    }
#endif
    return out;
}

} // namespace ulw::test
