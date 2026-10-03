#pragma once

#include "core/ports/auth.hpp"
#include "core/ports/catalog.hpp"
#include "core/ports/clock.hpp"
#include "core/ports/random.hpp"
#include "core/ports/storage.hpp"
#include "core/ports/views.hpp"
#include "net/ip_address.hpp"
#include "net/offload_pool.hpp"
#include "net/reactor.hpp"
#include "net/signals.hpp"
#include "net/slab.hpp"
#include "net/transport.hpp"

#include "bounded_table.hpp"
#include "health.hpp"
#include "live_manifest_cache.hpp"
#include "live_streams.hpp"
#include "ops/log.hpp"
#include "ops/metrics.hpp"
#include "rate_limit.hpp"
#include "view_recorder.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
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
    // The stream service (ADR-0091); none where live publishing is not configured, and its
    // routes then answer 404.
    LiveStreams* live_streams = nullptr;
};

struct Limits {
    // 600 MB left for connections on a 1 GB box / ~428 KB each (36 KB session and parser,
    // 92 KB pump, 52 KB TLS, measured; 128 KiB + 16 KiB kernel buffers, ~100 KB backend socket)
    // = 1401; a /3 safety factor gives 467, rounded down to 448 (docs/adr/0027).
    std::size_t max_connections = 448;
    // Chunk uploads are what cost the budget above, so admission counts them, not sockets.
    std::size_t max_upload_slots = 448;
    std::size_t max_uploads_per_user = 3;
    // Brief 8.13's per-client limits, counted per address (an IPv6 /64, see client_key). 20
    // connections hold a household behind one NAT: a browser opens up to 6 per origin, and
    // each user's uploader 3 PATCHes and a control request besides.
    std::size_t max_connections_per_ip = 20;
    // A client reuses its connections, so 10 new ones a second, saved up for at most one
    // second, is well above any browser's need and far below a flood's: every one the gateway
    // takes on costs it a TLS handshake.
    std::uint32_t new_connections_per_ip_per_second = 10;
    // One user's uploader at 100 Mbit/s finishes an 8 MiB chunk every 0.67 s, 90 PATCHes a
    // minute however many run at once; 300 leaves 210 more for playlists, polls and retries,
    // and caps one account at 300 x 8 MiB a minute, 40 MiB/s.
    std::uint32_t requests_per_user_per_minute = 300;
    // Two 50 GiB uploads, the largest there is, a day: one and a complete retry of it. At that
    // a user takes 100 GiB of the 600 Mbit/s port's 6.5 TB a day (75 MB/s x 86,400 s), 1.7%.
    // Refilled evenly, 100 GiB / 86,400 s = 1.2 MiB/s, so a refused 16 MiB PATCH waits 13 s.
    std::uint64_t upload_bytes_per_user_per_day = std::uint64_t{100} << 30U;
    // Peers whose X-Forwarded-For names the client (the Envoy data plane in front). None by
    // default: then every peer is the client itself, and a forged header changes nothing.
    std::vector<net::IpNetwork> trusted_proxies;
    // How many of them stand in front, each appending one X-Forwarded-For entry: the client is
    // that many entries from the right.
    std::size_t trusted_proxy_hops = 1;
    core::Millis header_timeout{10'000};
    core::Millis body_idle_timeout{30'000};
    // A chunk body must average at least this rate over each window the gateway spends reading
    // it, or it is answered 408; time the gateway is not reading, waiting on the store or the
    // catalog, does not count. Every upload part holds one of the store's connections (as many
    // as max_upload_slots) until its last byte, so without a floor clients sending a byte now
    // and then would hold them all for the six-hour backstop. At 8 KiB/s an 8 MiB part lets its
    // connection go within 8 MiB / 8 KiB/s = 1024 s. The floor is half of what each of a user's 3
    // concurrent uploads gets from a 384 kbit/s (UMTS) uplink: 48,000 B/s / 3 = 16,000 B/s, halved
    // for a link running at half its nominal rate.
    std::uint64_t min_body_bytes_per_second = std::uint64_t{8} * 1024;
    // Long enough to average out a mobile link's stalls and the slow start after each one.
    core::Millis body_rate_window{30'000};
    core::Millis request_backstop = std::chrono::duration_cast<core::Millis>(std::chrono::hours(6));
    core::Millis drain_deadline{30'000};
    std::size_t max_requests_per_connection = 1000;
    std::string auth_cookie = "auth_token";
    // The pages whose requests may carry the cookie, as scheme://host[:port]. A request with the
    // cookie and no Authorization must name one of them in Origin if its method changes
    // anything, and must not name another if it sends Origin at all. Empty: the cookie works
    // for GET and HEAD from this server's own origin only.
    std::vector<std::string> allowed_origins;
    // Whether a page on a sibling subdomain (Sec-Fetch-Site: same-site) may send the cookie.
    // Off, only this server's own origin may: a sibling can be a different, less trusted app.
    bool allow_same_site = false;
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
    LiveCacheLimits live_cache{};
};

struct Counters {
    std::uint64_t connections_accepted = 0;
    // The slab was full.
    std::uint64_t rejected_capacity = 0;
    // The socket refused its options, or the transport refused the socket.
    std::uint64_t rejected_socket = 0;
    // Accepted by the kernel after the drain began.
    std::uint64_t rejected_draining = 0;
    // The address already had max_connections_per_ip open, or opened them faster than
    // new_connections_per_ip_per_second.
    std::uint64_t rejected_ip_connections = 0;
    std::uint64_t rejected_ip_rate = 0;
    // Answered 429: a proxied client with max_connections_per_ip requests in flight, a user
    // past requests_per_user_per_minute, a PATCH past upload_bytes_per_user_per_day.
    std::uint64_t limited_ip_requests = 0;
    std::uint64_t limited_user_requests = 0;
    std::uint64_t limited_user_bytes = 0;
    // Answered 403 before the token was checked: a request with the cookie from a page the
    // gateway does not trust, or a create with the cookie that did not declare JSON.
    std::uint64_t cross_site_rejections = 0;
    std::uint64_t admission_rejections = 0;
    std::uint64_t timeouts_header = 0;
    std::uint64_t timeouts_body = 0;
    std::uint64_t timeouts_body_rate = 0;
    std::uint64_t timeouts_backstop = 0;
    std::uint64_t bytes_ingested = 0;
    std::uint64_t requests = 0;
    std::uint64_t certificate_reloads = 0;
    std::uint64_t certificate_reload_failures = 0;
    // SIGHUPs that requested a drop of the cached JWKS keys and remembered verified tokens;
    // each completes on the next successful key fetch (ADR-0082).
    std::uint64_t auth_cache_drops = 0;
    std::uint64_t playlists_master = 0;
    std::uint64_t playlists_media = 0;
    std::uint64_t playlists_live = 0;
    // A stored playlist broke a rewriting rule: the worker wrote something wrong.
    std::uint64_t playlists_rejected = 0;
    std::uint64_t presign_failures = 0;
    // By status class: 1xx, 2xx, 3xx, 4xx, 5xx.
    std::array<std::uint64_t, 5> responses{};
};

enum class Admission : std::uint8_t { Admitted, UserAtLimit, Full };

// A client address's hold on the gateway: its direct connection, or one request a trusted
// proxy relayed for it. While any is held the address's count cannot be forgotten.
using ClientHold = std::uint32_t;

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
    // Connections a drain lets finish: some of a request read, and not yet closed.
    [[nodiscard]] std::size_t busy_connections() noexcept;
    // Response bytes the connections hold that the kernel has not taken yet.
    [[nodiscard]] std::size_t queued_output() noexcept;
    // Bytes of new requests the connections hold unparsed behind a held-back response.
    [[nodiscard]] std::size_t held_bytes() noexcept;

    [[nodiscard]] const Deps& deps() const noexcept { return deps_; }
    [[nodiscard]] const Limits& limits() const noexcept { return limits_; }
    [[nodiscard]] Counters& counters() noexcept { return counters_; }
    [[nodiscard]] std::size_t upload_slots_in_use() const noexcept { return upload_slots_; }
    [[nodiscard]] ViewRecorder& views() noexcept { return views_; }
    [[nodiscard]] LiveManifestCache& live() noexcept { return live_; }
    // From a chunk's first byte handed to the store to the store holding all of it durably.
    [[nodiscard]] ops::Histogram& part_upload_duration() noexcept { return part_upload_; }
    // Each stretch a chunk's body waited because the store took nothing more.
    [[nodiscard]] ops::Histogram& backend_write_stall() noexcept { return write_stall_; }
    // Everything the gateway counts, in the text format metric scrapers read.
    [[nodiscard]] std::string render_metrics();

    [[nodiscard]] bool trusted_proxy(const net::IpAddress& peer) const noexcept;
    // nullopt when the client already holds max_connections_per_ip.
    [[nodiscard]] std::optional<ClientHold> hold_client(const net::IpAddress& client) noexcept;
    void release_client(ClientHold hold) noexcept;
    // A refusal says how long until the user's bucket would allow it.
    [[nodiscard]] std::expected<void, core::Millis>
    charge_request(const core::UserId& user) noexcept;
    [[nodiscard]] std::expected<void, core::Millis>
    charge_upload_bytes(const core::UserId& user, std::uint64_t bytes) noexcept;
    // Bytes charged for a PATCH body that never arrived.
    void refund_upload_bytes(const core::UserId& user, std::uint64_t bytes) noexcept;

    [[nodiscard]] Admission acquire_upload_slot(const core::UserId& user) noexcept;
    void release_upload_slot(const core::UserId& user) noexcept;
    void retire(net::Slab<Connection>::Handle handle) noexcept;
    // Discards an ingest nothing will ever reference, because its client left or the catalog
    // refused it. Owned here rather than by a connection, which may serve its next request
    // while the discard is still running.
    void abandon(const core::ports::IngestId& ingest) noexcept;

private:
    class Discard;

    struct ClientEntry {
        TokenBucket new_connections;
    };
    struct UserEntry {
        TokenBucket requests;
        TokenBucket upload_bytes;
    };

    // SIGHUP: refetches the JWKS, and asks the verifier to forget its keys and every
    // remembered verified token once that fetch succeeds.
    void drop_auth_caches() noexcept;
    // Refuses a direct peer at its limits, before a byte of it is read or a handshake begun.
    [[nodiscard]] std::optional<ClientHold> admit_peer(const net::IpAddress& peer) noexcept;
    [[nodiscard]] UserEntry* user_entry(const core::UserId& user) noexcept;

    Deps deps_;
    Limits limits_;
    Counters counters_;
    BucketRule new_connection_rule_;
    BucketRule request_rule_;
    BucketRule upload_byte_rule_;
    BoundedTable<net::IpAddress, ClientEntry, AddressHash> clients_;
    BoundedTable<core::UserId, UserEntry, UserHash> users_;
    net::Slab<Connection> connections_;
    std::unordered_map<core::UserId, std::size_t> uploads_by_user_;
    std::size_t upload_slots_ = 0;
    bool draining_ = false;
    net::TimerId drain_timer_;
    std::vector<std::unique_ptr<Discard>> discards_;
    ViewRecorder views_;
    LiveManifestCache live_;
    ops::Histogram part_upload_;
    ops::Histogram write_stall_;
};

} // namespace gateway
