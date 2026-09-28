#include "storage_harness.hpp"

#include "infra/storage/fs_store.hpp"
#include "net/offload_pool.hpp"
#include "net/reactor_factory.hpp"
#include "os/system_clock.hpp"
#include "os/system_random.hpp"

#include <cstdlib>
#include <filesystem>
#include <gtest/gtest.h>

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
        // Two threads: enough to overlap one session's write with another's control call.
        pool_ = std::move(*net::OffloadPool::create(*reactor_, 2));
        store_ = std::make_unique<infra::storage::FsStore>(
            infra::storage::FsStore::Deps{
                .reactor = *reactor_, .pool = *pool_, .clock = system_clock(), .random = random_},
            root_, kLocalChunk);
    }
    ~FsHarness() override {
        // The pool first: its threads may still be running jobs the store owns.
        pool_.reset();
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
        if (!pump_until(*reactor_, [&] { return pool_->in_flight() == 0; }, kOperationLimit)) {
            ADD_FAILURE() << "offload pool never drained";
        }
    }

private:
    std::filesystem::path root_;
    os::SystemRandom random_;
    std::unique_ptr<net::OffloadPool> pool_;
    std::unique_ptr<infra::storage::FsStore> store_;
};

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
    return out;
}

} // namespace ulw::test
