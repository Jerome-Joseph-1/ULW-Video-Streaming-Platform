#pragma once

#include "core/ports/auth.hpp"
#include "core/ports/catalog.hpp"
#include "core/ports/clock.hpp"
#include "core/ports/random.hpp"
#include "core/ports/storage.hpp"
#include "core/ports/views.hpp"
#include "net/offload_pool.hpp"
#include "net/reactor.hpp"
#include "net/signals.hpp"
#include "net/slab.hpp"
#include "net/transport.hpp"

#include "health.hpp"
#include "ops/log.hpp"
#include "ops/metrics.hpp"
#include "view_recorder.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace gateway {

class Connection;

struct Deps {
    net::IReactor& reactor;
    // Every client socket's bytes go through this, never straight to the reactor.
    net::ITransportFactory& transports;
    net::OffloadPool& pool;
    core::ports::IIngestStore& store;
    // Thread-safe: playlists are fetched and signed on the offload pool.
    core::ports::IObjectReader& reader;
    core::ports::IUploadCatalog& catalog;
    core::ports::IViewLog& views;
    core::ports::IJwtVerifier& verifier;
    const core::ports::IClock& clock;
    core::ports::IRandom& random;
    ops::Logger& log;
    // Written by the health probe's thread; only read here.
    const Health& health;
};

struct Limits {
    // 600 MB left for connections on a 1 GB box / ~428 KB each (36 KB session and parser,
    // 92 KB pump, 52 KB TLS, measured; 128 KiB + 16 KiB kernel buffers, ~100 KB backend socket)
    // = 1401; a /3 safety factor gives 467, rounded down to 448 (docs/adr/0027).
    std::size_t max_connections = 448;
    // Chunk uploads are what cost the budget above, so admission counts them, not sockets.
    std::size_t max_upload_slots = 448;
    std::size_t max_uploads_per_user = 3;
    core::Millis header_timeout{10'000};
    core::Millis body_idle_timeout{30'000};
    // A chunk body must average at least this rate over each window the gateway spends reading
    // it, or it is answered 408; time the store holds the body up does not count. Every upload
    // part holds one of the store's 64 connections until its last byte, so without a floor a
    // few dozen clients sending a byte now and then would hold them all for the six-hour
    // backstop. At 8 KiB/s an 8 MiB part lets its connection go within 8 MiB / 8 KiB/s =
    // 1024 s. The floor is half of what each of a user's 3 concurrent uploads gets from a
    // 384 kbit/s (UMTS) uplink: 48,000 B/s / 3 = 16,000 B/s, halved for a link running at
    // half its nominal rate.
    std::uint64_t min_body_bytes_per_second = std::uint64_t{8} * 1024;
    // Long enough to average out a mobile link's stalls and the slow start after each one.
    core::Millis body_rate_window{30'000};
    core::Millis request_backstop = std::chrono::duration_cast<core::Millis>(std::chrono::hours(6));
    core::Millis drain_deadline{30'000};
    std::size_t max_requests_per_connection = 1000;
    std::string auth_cookie = "auth_token";
    // Our reaper aborts abandoned uploads before the bucket's 7-day lifecycle rule does, so the
    // catalog never points at an ingest the store has already dropped.
    std::chrono::hours upload_ttl{6 * 24};
    // Filesystem backend only: the base URL a development file server publishes its objects
    // under, which segment URLs are then built on. Empty means no playback from that backend.
    std::string local_read_url;
    // A master playlist fetch is one view event, 160 bytes held. 1024 of them are 160 KB,
    // and at one write every 5 s they absorb 1024 / 5 = 204 new viewers a second on
    // one shard before any is dropped.
    std::size_t view_batch = 1024;
    core::Millis view_interval{5'000};
};

struct Counters {
    std::uint64_t connections_accepted = 0;
    // The slab was full.
    std::uint64_t rejected_capacity = 0;
    // The socket refused its options, or the transport refused the socket.
    std::uint64_t rejected_socket = 0;
    // Accepted by the kernel after the drain began.
    std::uint64_t rejected_draining = 0;
    std::uint64_t admission_rejections = 0;
    std::uint64_t timeouts_header = 0;
    std::uint64_t timeouts_body = 0;
    std::uint64_t timeouts_body_rate = 0;
    std::uint64_t timeouts_backend = 0;
    std::uint64_t timeouts_backstop = 0;
    std::uint64_t bytes_ingested = 0;
    std::uint64_t requests = 0;
    std::uint64_t certificate_reloads = 0;
    std::uint64_t certificate_reload_failures = 0;
    std::uint64_t playlists_master = 0;
    std::uint64_t playlists_media = 0;
    // A stored playlist broke a rewriting rule: the worker wrote something wrong.
    std::uint64_t playlists_rejected = 0;
    std::uint64_t presign_failures = 0;
    // By status class: 1xx, 2xx, 3xx, 4xx, 5xx.
    std::array<std::uint64_t, 5> responses{};
};

enum class Admission : std::uint8_t { Admitted, UserAtLimit, Full };

// One shard of the gateway: a listener, the connections it accepted and the admission
// counters for them. Everything runs on the shard's reactor thread.
class Gateway final : public net::IAcceptHandler,
                      public net::ISignalHandler,
                      public net::ITimerHandler,
                      public net::IReloadHandler {
public:
    Gateway(Deps deps, Limits limits);
    ~Gateway() override;
    Gateway(const Gateway&) = delete;
    Gateway& operator=(const Gateway&) = delete;

    void on_accept(os::UniqueFd conn) noexcept override;
    void on_signal(net::Signal signal) noexcept override;
    // The drain deadline.
    void on_timeout() noexcept override;
    void on_reloaded(const std::expected<void, std::string>& result) noexcept override;

    void begin_drain() noexcept;
    // Destroys connections the kernel and every pending callback have let go of. Call after
    // each run_once.
    void reap() noexcept;
    [[nodiscard]] bool finished() const noexcept {
        return draining_ && connections_.size() == 0 && views_.idle();
    }
    [[nodiscard]] bool draining() const noexcept { return draining_; }
    [[nodiscard]] std::size_t connections() const noexcept { return connections_.size(); }

    [[nodiscard]] const Deps& deps() const noexcept { return deps_; }
    [[nodiscard]] const Limits& limits() const noexcept { return limits_; }
    [[nodiscard]] Counters& counters() noexcept { return counters_; }
    [[nodiscard]] std::size_t upload_slots_in_use() const noexcept { return upload_slots_; }
    [[nodiscard]] ViewRecorder& views() noexcept { return views_; }
    // From a chunk's first byte handed to the store to the store holding all of it durably.
    [[nodiscard]] ops::Histogram& part_upload_duration() noexcept { return part_upload_; }
    // Each stretch a chunk's body waited because the store took nothing more.
    [[nodiscard]] ops::Histogram& backend_write_stall() noexcept { return write_stall_; }
    // Everything the gateway counts, in the text format metric scrapers read.
    [[nodiscard]] std::string render_metrics();

    [[nodiscard]] Admission acquire_upload_slot(const core::UserId& user) noexcept;
    void release_upload_slot(const core::UserId& user) noexcept;
    void retire(net::Slab<Connection>::Handle handle) noexcept;
    // Discards an ingest nothing will ever reference, because its client left or the catalog
    // refused it. Owned here rather than by a connection, which may serve its next request
    // while the discard is still running.
    void abandon(const core::ports::IngestId& ingest) noexcept;

private:
    class Discard;

    Deps deps_;
    Limits limits_;
    Counters counters_;
    net::Slab<Connection> connections_;
    std::unordered_map<core::UserId, std::size_t> uploads_by_user_;
    std::size_t upload_slots_ = 0;
    bool draining_ = false;
    net::TimerId drain_timer_;
    std::vector<std::unique_ptr<Discard>> discards_;
    ViewRecorder views_;
    ops::Histogram part_upload_;
    ops::Histogram write_stall_;
};

} // namespace gateway
