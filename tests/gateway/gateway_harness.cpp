#include "gateway_harness.hpp"

#include "infra/storage/fs_store.hpp"
#include "infra/storage/s3_store.hpp"
#include "net/offload_pool.hpp"
#include "net/reactor_factory.hpp"
#include "net/socket.hpp"
#include "os/system_clock.hpp"
#include "os/system_random.hpp"

#include "../conformance/storage_harness.hpp"
#include "support/eventually.hpp"
#include "support/fake_clock.hpp"
#include "support/fake_verifier.hpp"
#include "support/live_s3.hpp"
#include "support/tls_pki.hpp"

#include <sys/eventfd.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <gtest/gtest.h>
#include <mutex>
#include <unistd.h>

namespace ulw::test {

struct GatewayUnderTest::Loop final : net::IReadyHandler {
    explicit Loop(MemoryLog& sink) : log(sink, system_clock, "gateway", ops::Level::Debug) {}

    os::SystemClock system_clock;
    ops::Logger log;
    gateway::Health health;
    std::optional<bool> database_up;
    std::optional<bool> store_up;
    FakeClock manual_clock;
    core::ports::IClock* clock = &system_clock;
    os::SystemRandom random;
    std::unique_ptr<net::IReactor> reactor;
    std::unique_ptr<net::ITransportFactory> transports;
    std::unique_ptr<net::OffloadPool> pool;
    std::unique_ptr<infra::storage::FakeStore> fake;
    std::unique_ptr<infra::storage::FsStore> fs;
    ulw::test::LiveS3 store_target = minio_from_env();
    std::unique_ptr<infra::curl::Multi> multi;
    std::unique_ptr<infra::storage::S3Store> s3;
    core::ports::IIngestStore* store = nullptr;
    core::ports::IObjectAdmin* admin = nullptr;
    std::unique_ptr<infra::catalog::MemoryCatalog> catalog;
    FakeVerifier verifier;
    std::unique_ptr<gateway::Gateway> gateway;
    std::filesystem::path root;

    os::UniqueFd wake{::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)};
    std::mutex mutex;
    std::deque<std::packaged_task<void()>> tasks;
    std::atomic<bool> stop{false};

    void on_ready(net::Interest /*ready*/) noexcept override {
        std::uint64_t n = 0;
        [[maybe_unused]] const ssize_t got = ::read(wake.get(), &n, sizeof n);
        std::deque<std::packaged_task<void()>> batch;
        {
            const std::scoped_lock lock(mutex);
            batch.swap(tasks);
        }
        for (auto& t : batch) {
            t();
        }
    }

    void post(std::packaged_task<void()> task) {
        {
            const std::scoped_lock lock(mutex);
            tasks.push_back(std::move(task));
        }
        const std::uint64_t one = 1;
        [[maybe_unused]] const ssize_t put = ::write(wake.get(), &one, sizeof one);
    }
};

GatewayUnderTest::GatewayUnderTest(GatewayOptions options) : loop_(std::make_unique<Loop>(*log_)) {
    if (options.transport == gateway::Transport::Tls) {
        client_tls_ = TestPki::shared().client_context();
    }
    std::promise<void> ready;
    auto started = ready.get_future();
    thread_ =
        std::jthread([this, options = std::move(options), ready = std::move(ready)]() mutable {
            run(options, std::move(ready));
        });
    started.get();
}

GatewayUnderTest::~GatewayUnderTest() {
    // Set from the loop itself: flipping it from here could let the loop exit before running
    // the task that is supposed to wake it, and this would wait forever.
    on_loop([this] { loop_->stop = true; });
    thread_.join();
}

void GatewayUnderTest::run(const GatewayOptions& options, std::promise<void> ready) {
    Loop& l = *loop_;
    if (options.manual_clock) {
        l.clock = &l.manual_clock;
    }
    l.database_up = options.database_up;
    l.store_up = options.store_up;
    const auto probe = [&l] {
        if (l.database_up && l.store_up) {
            l.health.record(*l.database_up, *l.store_up, l.clock->now());
        }
    };
    probe();
    l.reactor = std::move(*net::make_reactor(reactor_kind_from_env(), *l.clock, 4096));
    if (options.transport == gateway::Transport::Tls) {
        auto tls = net::make_tls_transports(*l.reactor,
                                            options.tls_files.value_or(TestPki::shared().server()));
        if (!tls) {
            static_cast<void>(std::fputs("gateway harness: tls transport refused\n", stderr));
            std::abort();
        }
        l.transports = std::move(*tls);
    } else {
        l.transports = net::make_plain_transports(*l.reactor);
    }
    l.pool = std::move(*net::OffloadPool::create(*l.reactor, 4));
    switch (options.backend) {
    case Backend::Fake:
        l.fake = std::make_unique<infra::storage::FakeStore>(*l.reactor, *l.clock, options.chunk,
                                                             options.plan);
        fake_ = l.fake.get();
        reader_ = l.fake.get();
        l.store = l.fake.get();
        l.admin = l.fake.get();
        break;
    case Backend::Fs: {
        std::string tmpl = (std::filesystem::temp_directory_path() / "ulw-gw-XXXXXX").string();
        l.root = ::mkdtemp(tmpl.data());
        l.fs = std::make_unique<infra::storage::FsStore>(
            infra::storage::FsStore::Deps{.clock = *l.clock, .random = l.random},
            std::move(*net::OffloadPool::create(*l.reactor, 2)), l.root, options.chunk);
        reader_ = l.fs.get();
        l.store = l.fs.get();
        l.admin = l.fs.get();
        break;
    }
    case Backend::S3: {
        auto multi = infra::curl::Multi::create(*l.reactor, options.store_connections,
                                                options.store_stall_limit);
        if (!multi) {
            static_cast<void>(std::fputs("gateway harness: curl multi refused\n", stderr));
            std::abort();
        }
        l.multi = std::move(*multi);
        if (options.store_endpoint) {
            l.store_target.profile = *infra::s3util::S3Profile::minio(*options.store_endpoint);
        }
        // Signed with the real time whatever the loop's clock says: MinIO refuses a request
        // dated more than 15 minutes from its own.
        auto s3 = infra::storage::S3Store::create(
            infra::storage::S3Store::Deps{.reactor = *l.reactor,
                                          .multi = *l.multi,
                                          .credentials = l.store_target.credentials,
                                          .clock = l.system_clock,
                                          .random = l.random,
                                          .profile = l.store_target.profile,
                                          .bucket = l.store_target.bucket},
            {.part_size = options.chunk});
        if (!s3) {
            static_cast<void>(std::fputs("gateway harness: s3 store refused\n", stderr));
            std::abort();
        }
        l.s3 = std::move(*s3);
        reader_ = l.s3.get();
        l.store = l.s3.get();
        l.admin = l.s3.get();
        break;
    }
    }
    l.catalog = std::make_unique<infra::catalog::MemoryCatalog>(*l.reactor);
    l.gateway = std::make_unique<gateway::Gateway>(gateway::Deps{.reactor = *l.reactor,
                                                                 .transports = *l.transports,
                                                                 .pool = *l.pool,
                                                                 .store = *l.store,
                                                                 .reader = *reader_,
                                                                 .catalog = *l.catalog,
                                                                 .views = *l.catalog,
                                                                 .verifier = l.verifier,
                                                                 .clock = *l.clock,
                                                                 .random = l.random,
                                                                 .log = l.log,
                                                                 .health = l.health},
                                                   options.limits);
    auto listener = net::listen_tcp({.port = 0, .loopback_only = true});
    port_ = *net::local_port(listener->get());
    if (!l.reactor->listen(std::move(*listener), *l.gateway) ||
        !l.reactor->watch(l.wake.get(), net::Interest::Read, l)) {
        // Nothing a test could do without a listening gateway.
        static_cast<void>(std::fputs("gateway harness: listen or watch failed\n", stderr));
        std::abort();
    }
    ready.set_value();

    while (!l.stop) {
        l.reactor->run_once(core::Millis{50});
        l.gateway->reap();
        probe();
    }
    l.reactor->unwatch(l.wake.get());
    // The pool first: a job it is running points at a connection the gateway owns.
    l.pool.reset();
    l.gateway.reset();
    l.fs.reset();
    l.fake.reset();
    // Its sessions went with the gateway's connections; its transfers with them.
    l.s3.reset();
    l.multi.reset();
    l.catalog.reset();
    l.transports.reset();
    l.reactor.reset();
    if (!l.root.empty()) {
        std::error_code ec;
        std::filesystem::remove_all(l.root, ec);
    }
}

void GatewayUnderTest::on_loop(std::function<void()> fn) {
    std::packaged_task<void()> task(std::move(fn));
    auto done = task.get_future();
    loop_->post(std::move(task));
    done.get();
}

void GatewayUnderTest::set_plan(const infra::storage::FaultPlan& plan) {
    fake_->set_plan(plan);
}

void GatewayUnderTest::hold_fetches(bool held) {
    fake_->hold_fetches(held);
}

std::size_t GatewayUnderTest::held_fetches() const {
    return fake_->held_fetches();
}

bool GatewayUnderTest::finished() {
    bool out = false;
    on_loop([&] { out = loop_->gateway->finished(); });
    return out;
}

std::vector<infra::catalog::MemoryCatalog::Job> GatewayUnderTest::jobs() {
    std::vector<infra::catalog::MemoryCatalog::Job> out;
    on_loop([&] { out = loop_->catalog->jobs(); });
    return out;
}

gateway::Counters GatewayUnderTest::counters() {
    gateway::Counters out;
    on_loop([&] { out = loop_->gateway->counters(); });
    return out;
}

std::size_t GatewayUnderTest::connections() {
    std::size_t out = 0;
    on_loop([&] { out = loop_->gateway->connections(); });
    return out;
}

std::size_t GatewayUnderTest::claims() {
    std::size_t out = 0;
    on_loop([&] { out = loop_->catalog->claims(); });
    return out;
}

void GatewayUnderTest::drain() {
    on_loop([&] { loop_->gateway->begin_drain(); });
}

void GatewayUnderTest::advance(core::Millis d) {
    on_loop([&] { loop_->manual_clock.advance(d); });
    // The turn that ran the task above read the clock before it moved, so the timers it made
    // due fire at the end of the next turn. A task run after that turn has seen them fire, and
    // bytes the test sends next cannot land ahead of them.
    on_loop([] {});
    on_loop([] {});
}

void GatewayUnderTest::refresh_keys() {
    on_loop([&] { loop_->verifier.refresh_keys(); });
}

std::size_t GatewayUnderTest::key_waiters() {
    std::size_t out = 0;
    on_loop([&] { out = loop_->verifier.waiting(); });
    return out;
}

void GatewayUnderTest::reload_certificate() {
    const auto finished = [](const gateway::Counters& c) {
        return c.certificate_reloads + c.certificate_reload_failures;
    };
    const std::uint64_t before = finished(counters());
    on_loop([&] { loop_->gateway->on_signal(net::Signal::Reload); });
    if (!eventually([&] { return finished(counters()) > before; })) {
        ADD_FAILURE() << "certificate reload never finished";
    }
}

void GatewayUnderTest::put_video(const core::VideoRecord& video) {
    on_loop([&] { loop_->catalog->put_video(video); });
}

void GatewayUnderTest::put_object(std::string_view key, std::string_view bytes) {
    core::ports::IObjectAdmin& admin = *loop_->admin;
    const auto parsed = core::StorageKey::parse(key);
    ASSERT_TRUE(parsed) << key;
    ASSERT_TRUE(admin.put(*parsed, std::as_bytes(std::span(bytes))));
}

std::vector<core::ports::ViewEvent> GatewayUnderTest::views() {
    std::vector<core::ports::ViewEvent> out;
    on_loop([&] { out = loop_->catalog->views(); });
    return out;
}

void GatewayUnderTest::fail_views(std::optional<core::ports::CatalogError> error) {
    on_loop([&] { loop_->catalog->fail_views(error); });
}

std::string GatewayUnderTest::metrics() {
    std::string out;
    on_loop([&] { out = loop_->gateway->render_metrics(); });
    return out;
}

void GatewayUnderTest::set_health(std::optional<bool> database_up, std::optional<bool> store_up) {
    on_loop([&] {
        loop_->database_up = database_up;
        loop_->store_up = store_up;
    });
    // Two turns after this one have probed with the new answers: a dependency takes two
    // failed probes in a row to count as down.
    on_loop([] {});
    on_loop([] {});
    on_loop([] {});
}

} // namespace ulw::test
