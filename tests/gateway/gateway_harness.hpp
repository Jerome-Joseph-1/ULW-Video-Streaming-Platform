#pragma once

#include "infra/catalog/memory_catalog.hpp"
#include "infra/storage/fake_store.hpp"

#include "config.hpp"
#include "gateway.hpp"
#include "support/http_client.hpp"

#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <thread>

namespace ulw::test {

enum class Backend { Fake, Fs };

struct GatewayOptions {
    Backend backend = Backend::Fs;
    std::uint64_t chunk = std::uint64_t{8} * 1024 * 1024;
    infra::storage::FaultPlan plan{};
    gateway::Limits limits{};
    // Time moves only when the test calls advance(), so timeouts need no waiting.
    bool manual_clock = false;
    gateway::Transport transport = gateway::Transport::Plain;
    // TLS only: the files served, the process-wide test identity when unset.
    std::optional<net::TlsFiles> tls_files = std::nullopt;
};

// A gateway shard on its own reactor thread, as in production, reachable over loopback.
// Everything the gateway owns is created, used and destroyed on that thread.
class GatewayUnderTest {
public:
    explicit GatewayUnderTest(GatewayOptions options);
    ~GatewayUnderTest();
    GatewayUnderTest(const GatewayUnderTest&) = delete;
    GatewayUnderTest& operator=(const GatewayUnderTest&) = delete;

    [[nodiscard]] std::uint16_t port() const { return port_; }
    // The port, and for TLS a client context that trusts the gateway's certificate.
    [[nodiscard]] Endpoint endpoint() const { return {.port = port_, .tls = client_tls_.get()}; }
    // Thread-safe reads of committed objects.
    [[nodiscard]] core::ports::IObjectReader& reader() { return *reader_; }
    void set_plan(const infra::storage::FaultPlan& plan);

    // Runs `fn` on the loop thread and waits for it.
    void on_loop(std::function<void()> fn);
    [[nodiscard]] std::vector<infra::catalog::MemoryCatalog::Job> jobs();
    [[nodiscard]] gateway::Counters counters();
    [[nodiscard]] std::size_t connections();
    [[nodiscard]] std::size_t claims();
    void drain();
    // Manual clock only: moves time forward on the loop thread and returns once the timers
    // it made due have fired.
    void advance(core::Millis d);
    // Lets every connection waiting on an unknown signing key ("slow." tokens) continue.
    void refresh_keys();
    [[nodiscard]] std::size_t key_waiters();
    // What SIGHUP does: reread the certificate and key. Returns once the reload has finished.
    void reload_certificate();
    [[nodiscard]] std::string metrics();

private:
    struct Loop;
    void run(const GatewayOptions& options, std::promise<void> ready);

    std::unique_ptr<Loop> loop_;
    std::uint16_t port_ = 0;
    SslCtxPtr client_tls_;
    core::ports::IObjectReader* reader_ = nullptr;
    infra::storage::FakeStore* fake_ = nullptr;
    std::jthread thread_;
};

} // namespace ulw::test
