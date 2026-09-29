#include "gateway_harness.hpp"

#include "infra/storage/fs_store.hpp"
#include "net/offload_pool.hpp"
#include "net/reactor_factory.hpp"
#include "net/socket.hpp"
#include "os/system_clock.hpp"
#include "os/system_random.hpp"

#include "../conformance/storage_harness.hpp"
#include "support/eventually.hpp"
#include "support/fake_clock.hpp"
#include "support/fake_verifier.hpp"
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
    if (options.backend == Backend::Fake) {
        l.fake = std::make_unique<infra::storage::FakeStore>(*l.reactor, *l.clock, options.chunk,
                                                             options.plan);
        fake_ = l.fake.get();
        reader_ = l.fake.get();
    } else {
        std::string tmpl = (std::filesystem::temp_directory_path() / "ulw-gw-XXXXXX").string();
        l.root = ::mkdtemp(tmpl.data());
        l.fs = std::make_unique<infra::storage::FsStore>(
            infra::storage::FsStore::Deps{.clock = *l.clock, .random = l.random},
            std::move(*net::OffloadPool::create(*l.reactor, 2)), l.root, options.chunk);
        reader_ = l.fs.get();
    }
    l.catalog = std::make_unique<infra::catalog::MemoryCatalog>(*l.reactor);
    core::ports::IIngestStore& store =
        l.fake ? static_cast<core::ports::IIngestStore&>(*l.fake) : *l.fs;
    l.gateway = std::make_unique<gateway::Gateway>(gateway::Deps{.reactor = *l.reactor,
                                                                 .transports = *l.transports,
                                                                 .pool = *l.pool,
                                                                 .store = store,
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
    core::ports::IObjectAdmin& admin =
        loop_->fake ? static_cast<core::ports::IObjectAdmin&>(*loop_->fake) : *loop_->fs;
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
    // The turn after this one has probed with the new answers.
    on_loop([] {});
}

} // namespace ulw::test
