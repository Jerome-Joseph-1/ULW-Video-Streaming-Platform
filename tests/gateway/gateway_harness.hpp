#pragma once

#include "infra/catalog/memory_catalog.hpp"
#include "infra/curl/multi.hpp"
#include "infra/storage/fake_store.hpp"

#include "config.hpp"
#include "gateway.hpp"
#include "health.hpp"
#include "support/http_client.hpp"
#include "support/memory_log.hpp"

#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace ulw::test {

// S3 is the store the gateway runs in production, on the MinIO of deploy/local/compose.yaml.
enum class Backend { Fake, Fs, S3 };

inline constexpr std::string_view kAllowedOrigin = "https://app.example";

// Production limits but the per-client ones, lifted: every test connects from 127.0.0.1, most as
// one user, and under a manual clock a bucket never refills. Tests of those limits set them.
[[nodiscard]] inline gateway::Limits unthrottled_limits() {
    gateway::Limits limits;
    limits.max_connections_per_ip = limits.max_connections;
    limits.new_connections_per_ip_per_second = 1'000'000;
    limits.requests_per_user_per_minute = 1'000'000;
    limits.upload_bytes_per_user_per_day = std::uint64_t{1} << 50U;
    // The page the tests' cookie requests come from, as a browser's Origin names it.
    limits.allowed_origins = {std::string(kAllowedOrigin)};
    return limits;
}

struct GatewayOptions {
    Backend backend = Backend::Fs;
    std::uint64_t chunk = std::uint64_t{8} * 1024 * 1024;
    infra::storage::FaultPlan plan{};
    gateway::Limits limits = unthrottled_limits();
    // S3 only: the store's connections, which uploads beyond wait for; the gateway's own
    // choice, one per admitted upload, unless a test wants them to queue.
    std::size_t store_connections = gateway::Limits{}.max_upload_slots;
    // S3 only: how long a part may move under a byte a second before the store fails it.
    std::chrono::seconds store_stall_limit = infra::curl::Multi::kDefaultStallLimit;
    // S3 only: a scripted peer's base URL in place of the MinIO of deploy/local/compose.yaml.
    std::optional<std::string> store_endpoint = std::nullopt;
    // Time moves only when the test calls advance(), so timeouts need no waiting.
    bool manual_clock = false;
    gateway::Transport transport = gateway::Transport::Plain;
    // TLS only: the files served, the process-wide test identity when unset.
    std::optional<net::TlsFiles> tls_files = std::nullopt;
    // What the health probe finds, refreshed every loop turn as if it probed that often.
    // Neither set: no probe has finished yet.
    std::optional<bool> database_up = true;
    std::optional<bool> store_up = true;
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
    // Sets the plan and tells every writer the store refused to try again: a store that
    // takes bytes again after holding a body up. Returns once the plan is in place.
    void resume_store(const infra::storage::FaultPlan& plan);
    // Fake backend only: store reads wait while held (FakeStore::hold_fetches).
    void hold_fetches(bool held);
    [[nodiscard]] std::size_t held_fetches() const;
    // The gateway has let every connection go after a drain.
    [[nodiscard]] bool finished();

    // Runs `fn` on the loop thread and waits for it.
    void on_loop(std::function<void()> fn);
    [[nodiscard]] std::vector<infra::catalog::MemoryCatalog::Job> jobs();
    [[nodiscard]] gateway::Counters counters();
    [[nodiscard]] std::size_t connections();
    // Connections the gateway has read part of a request from; accepted alone is not enough.
    [[nodiscard]] std::size_t busy_connections();
    [[nodiscard]] std::size_t claims();
    // The claims the gateway's requests hold, by its own count: catalog_claims_held.
    [[nodiscard]] std::size_t claims_held();
    void drain();
    // Manual clock only: moves time forward on the loop thread and returns once the timers
    // it made due have fired.
    void advance(core::Millis d);
    // Lets every connection waiting on an unknown signing key ("slow." tokens) continue.
    void refresh_keys();
    // Both in one turn of the loop: a response the key refresh lets through is sent, and the
    // drain begins, before the loop does anything else.
    void refresh_keys_then_drain();
    // Response bytes the gateway holds that its kernel has not taken yet.
    [[nodiscard]] std::size_t queued_output();
    // Bytes of new requests the gateway holds unparsed behind a held-back response.
    [[nodiscard]] std::size_t held_bytes();
    [[nodiscard]] std::size_t key_waiters();
    // What SIGHUP does: reread the certificate and key. Returns once the reload has finished.
    void reload_certificate();
    // SIGHUP itself, on the loop: also starts a certificate reload, which this does not await.
    void sighup();
    // How often the gateway told its verifier to drop its caches.
    [[nodiscard]] std::size_t verifier_drops();
    // What the verifier reports as drop_pending() from now on.
    void set_drop_pending(bool pending);
    [[nodiscard]] std::string metrics();
    // What the probe finds from the next loop turn on; nullopt stops it probing at all.
    void set_health(std::optional<bool> database_up, std::optional<bool> store_up);
    // Everything the gateway logged, at every level.
    [[nodiscard]] const MemoryLog& log() const { return *log_; }

    // Playback starts where the worker leaves off: a video row and its published objects.
    void put_video(const core::VideoRecord& video);
    // Thread-safe, as the stores' administrative calls are.
    void put_object(std::string_view key, std::string_view bytes);
    [[nodiscard]] std::vector<core::ports::ViewEvent> views();
    void fail_views(std::optional<core::ports::CatalogError> error);
    // Every upload and video call to the catalog fails with `error` from now on; nullopt stops it.
    void fail_catalog(std::optional<core::ports::CatalogError> error);
    void fail_find_video(std::optional<core::ports::CatalogError> error);
    // Claims are taken at once but their answers wait while held (MemoryCatalog::hold_claims).
    void hold_claims(bool held);
    [[nodiscard]] std::size_t held_claims();

private:
    struct Loop;
    void run(const GatewayOptions& options, std::promise<void> ready);

    std::unique_ptr<MemoryLog> log_ = std::make_unique<MemoryLog>();
    std::unique_ptr<Loop> loop_;
    std::uint16_t port_ = 0;
    SslCtxPtr client_tls_;
    core::ports::IObjectReader* reader_ = nullptr;
    infra::storage::FakeStore* fake_ = nullptr;
    std::jthread thread_;
};

} // namespace ulw::test
