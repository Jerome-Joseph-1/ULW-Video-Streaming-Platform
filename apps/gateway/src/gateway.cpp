#include "gateway.hpp"

#include "core/version.hpp"
#include "net/socket.hpp"

#include "connection.hpp"
#include "ops/metrics.hpp"

#include <array>
#include <limits>
#include <string_view>

namespace gateway {

namespace {

// An 8 MiB chunk takes 0.67 s at 100 Mbit/s, 6.7 s at 10 Mbit/s, 67 s at 1 Mbit/s, and
// 1024 s at the 8 KiB/s floor below which the gateway ends it.
constexpr std::array kPartUploadBuckets{0.25, 0.5,  1.0,   2.5,   5.0,   10.0,
                                        30.0, 60.0, 120.0, 300.0, 600.0, 1200.0};
// A healthy store takes the next buffer within milliseconds. A store that takes nothing at all
// is failed by libcurl after 60 s under a byte a second (ADR-0045), about 65 s with its rate
// window; a stall past 300 s is a store trickling, which only the backstop ends.
constexpr std::array kStallBuckets{0.005, 0.01, 0.025, 0.05, 0.1,  0.25, 0.5,
                                   1.0,   2.5,  5.0,   10.0, 30.0, 60.0, 300.0};

} // namespace

class Gateway::Discard final : public net::IOffloadJob {
public:
    Discard(core::ports::IIngestStore& store, core::ports::IngestId ingest)
        : store_(store), ingest_(std::move(ingest)) {}

    void run() noexcept override { store_.discard(ingest_); }
    // Destroyed by the next reap(), never from inside the pool's completion loop.
    void complete() noexcept override { done_ = true; }
    [[nodiscard]] bool done() const noexcept { return done_; }

private:
    core::ports::IIngestStore& store_;
    core::ports::IngestId ingest_;
    bool done_ = false;
};

Gateway::Gateway(Deps deps, Limits limits)
    : deps_(deps), limits_(std::move(limits)), connections_(limits_.max_connections),
      views_(deps_.reactor, deps_.views, limits_.view_batch, limits_.view_interval),
      part_upload_(kPartUploadBuckets), write_stall_(kStallBuckets) {}

Gateway::~Gateway() {
    deps_.reactor.cancel_timer(drain_timer_);
}

void Gateway::on_accept(os::UniqueFd conn) noexcept {
    if (draining_) {
        ++counters_.rejected_draining;
        return;
    }
    // A socket that refuses its options is already broken.
    if (!net::tune_connection(conn.get())) {
        ++counters_.rejected_socket;
        return;
    }
    const auto handle = connections_.emplace(*this);
    if (!handle) {
        ++counters_.rejected_capacity;
        return;
    }
    Connection* c = connections_.get(*handle);
    auto transport = deps_.transports.attach(std::move(conn), *c);
    if (!transport) {
        ++counters_.rejected_socket;
        connections_.retire(*handle);
        return;
    }
    ++counters_.connections_accepted;
    c->start(std::move(*transport));
}

void Gateway::on_signal(net::Signal signal) noexcept {
    switch (signal) {
    case net::Signal::Terminate:
        begin_drain();
        return;
    case net::Signal::Reload:
        deps_.transports.reload(deps_.pool, *this);
        return;
    }
}

// A renewed certificate is picked up without dropping a connection; a bad one is reported and
// the old one stays in service.
void Gateway::on_reloaded(const std::expected<void, std::string>& result) noexcept {
    if (result) {
        ++counters_.certificate_reloads;
        deps_.log.info("certificate reloaded");
        return;
    }
    ++counters_.certificate_reload_failures;
    deps_.log.error("certificate reload failed, keeping the old one", {{"error", result.error()}});
}

void Gateway::begin_drain() noexcept {
    if (draining_) {
        return;
    }
    // /readyz answers 503 from here on, so the load balancer stops sending new work while
    // requests in flight finish.
    draining_ = true;
    deps_.log.info("drain started", {{"connections", connections_.size()},
                                     {"deadline_ms", limits_.drain_deadline.count()}});
    deps_.reactor.stop_listening();
    connections_.for_each_live([](Connection& c) { c.drain(); });
    views_.drain();
    drain_timer_ = deps_.reactor.arm_timer(limits_.drain_deadline, *this);
}

// Whatever is still running now is cut off: the process is about to be killed anyway, and a
// request the client can retry is better ended by us than by SIGKILL.
void Gateway::on_timeout() noexcept {
    drain_timer_ = {};
    deps_.log.warn("drain deadline passed", {{"aborted", connections_.size()}});
    connections_.for_each_live([](Connection& c) { c.abort(); });
}

void Gateway::reap() noexcept {
    connections_.reap([](Connection& c) { return c.quiescent(); });
    std::erase_if(discards_, [](const auto& d) { return d->done(); });
}

Admission Gateway::acquire_upload_slot(const core::UserId& user) noexcept {
    if (upload_slots_ >= limits_.max_upload_slots) {
        return Admission::Full;
    }
    std::size_t& mine = uploads_by_user_[user];
    if (mine >= limits_.max_uploads_per_user) {
        if (mine == 0) {
            uploads_by_user_.erase(user);
        }
        return Admission::UserAtLimit;
    }
    ++mine;
    ++upload_slots_;
    return Admission::Admitted;
}

void Gateway::abandon(const core::ports::IngestId& ingest) noexcept {
    discards_.push_back(std::make_unique<Discard>(deps_.store, ingest));
    deps_.pool.submit(*discards_.back());
}

void Gateway::release_upload_slot(const core::UserId& user) noexcept {
    const auto it = uploads_by_user_.find(user);
    if (it == uploads_by_user_.end()) {
        return;
    }
    if (--it->second == 0) {
        uploads_by_user_.erase(it);
    }
    --upload_slots_;
}

void Gateway::retire(net::Slab<Connection>::Handle handle) noexcept {
    connections_.retire(handle);
}

std::string Gateway::render_metrics() {
    const Counters& c = counters_;
    const ViewCounters& v = views_.counters();
    const Health& h = deps_.health;
    std::uint64_t buffered = 0;
    connections_.for_each_live([&](const Connection& conn) { buffered += conn.buffered_bytes(); });
    ops::Exposition e;
    using ops::MetricType;

    const core::BuildInfo info = core::build_info();
    e.family("build_info", "The version and commit this binary was built from.", MetricType::Gauge);
    e.sample(
        "build_info",
        {{.name = "version", .value = info.version}, {.name = "git_sha", .value = info.git_sha}},
        std::uint64_t{1});
    e.gauge("ready", "1 while /readyz answers 200.",
            std::uint64_t{!draining_ && h.readiness(deps_.reactor.now()) == Readiness::Ready});
    e.family("dependency_up", "Whether the last health probe reached the dependency.",
             MetricType::Gauge);
    e.sample("dependency_up", {{.name = "dependency", .value = "database"}},
             std::uint64_t{h.database_up()});
    e.sample("dependency_up", {{.name = "dependency", .value = "store"}},
             std::uint64_t{h.store_up()});

    e.counter("requests_total", "Requests whose head was parsed.", c.requests);
    e.family("responses_total", "Responses sent, by status class.", MetricType::Counter);
    constexpr std::array<std::string_view, 5> kClasses{"1xx", "2xx", "3xx", "4xx", "5xx"};
    for (std::size_t i = 0; i < kClasses.size(); ++i) {
        e.sample("responses_total", {{.name = "class", .value = kClasses.at(i)}},
                 c.responses.at(i));
    }
    e.counter("connections_accepted_total", "Connections taken on.", c.connections_accepted);
    e.family("connections_rejected_total", "Connections closed as soon as they were accepted.",
             MetricType::Counter);
    e.sample("connections_rejected_total", {{.name = "reason", .value = "capacity"}},
             c.rejected_capacity);
    e.sample("connections_rejected_total", {{.name = "reason", .value = "socket"}},
             c.rejected_socket);
    e.sample("connections_rejected_total", {{.name = "reason", .value = "draining"}},
             c.rejected_draining);
    e.gauge("connections_current", "Connections open now.", connections_.size());
    e.gauge("uploads_in_flight", "Chunk uploads holding an admission slot.", upload_slots_);
    e.counter("admission_rejections_total",
              "Chunk uploads refused a slot, for the user's limit or the process's.",
              c.admission_rejections);
    e.counter("bytes_ingested_total", "Upload body bytes read from clients.", c.bytes_ingested);
    e.histogram("part_upload_duration_seconds",
                "From a chunk's first byte handed to the store to all of it durable.",
                part_upload_);
    e.histogram(
        "backend_write_stall_seconds",
        "Each wait of a chunk body on a store that took nothing more, observed when it ends.",
        write_stall_);
    e.gauge("buffer_bytes_in_use", "Bytes held in connections' staging and body buffers.",
            buffered);
    e.family("timeouts_total", "Requests or connections ended by a timer.", MetricType::Counter);
    e.sample("timeouts_total", {{.name = "kind", .value = "header"}}, c.timeouts_header);
    e.sample("timeouts_total", {{.name = "kind", .value = "body"}}, c.timeouts_body);
    e.sample("timeouts_total", {{.name = "kind", .value = "body_rate"}}, c.timeouts_body_rate);
    e.sample("timeouts_total", {{.name = "kind", .value = "backstop"}}, c.timeouts_backstop);
    e.gauge("tls_handshakes_in_flight", "TLS handshakes begun and not finished.",
            deps_.transports.handshakes_in_flight());
    e.counter("tls_handshake_failures_total", "TLS handshakes that failed or timed out.",
              deps_.transports.handshake_failures());
    e.counter("certificate_reloads_total", "Certificates reloaded on SIGHUP.",
              c.certificate_reloads);
    e.counter("certificate_reload_failures_total",
              "SIGHUP reloads refused; the previous certificate stayed in service.",
              c.certificate_reload_failures);
    e.family("playlist_requests_total", "Playlist requests, by kind.", MetricType::Counter);
    e.sample("playlist_requests_total", {{.name = "kind", .value = "master"}}, c.playlists_master);
    e.sample("playlist_requests_total", {{.name = "kind", .value = "media"}}, c.playlists_media);
    e.counter("playlists_rejected_total", "Stored playlists that broke a rewriting rule.",
              c.playlists_rejected);
    e.counter("presign_failures_total", "Segment URLs the store could not sign.",
              c.presign_failures);
    e.counter("view_events_recorded_total", "View events written.", v.recorded);
    e.counter("view_events_dropped_total", "View events refused or lost with a failed batch.",
              v.dropped);
    e.counter("view_batches_failed_total", "View batches the database refused.", v.failed_batches);
    // NaN while the database is out of reach: a frozen last value would read as a queue that
    // stopped aging, and 0 as an empty one.
    const auto oldest = h.oldest_queued_seconds();
    e.gauge("jobs_oldest_queued_seconds",
            "How long the oldest transcode job due to run has waited; 0 when none waits, NaN "
            "when the database did not answer.",
            oldest ? static_cast<double>(*oldest) : std::numeric_limits<double>::quiet_NaN());
    e.counter("store_paging_errors_total",
              "Store failures only a fix on our side cures: signature, credentials, bucket.",
              h.store_paging_errors());
    e.counter("log_messages_dropped_total", "Log lines dropped because the log reader lagged.",
              deps_.log.dropped());
    e.gauge("open_fds", "Descriptors open in the process.", h.open_fds());
    e.gauge("resident_memory_bytes", "Resident set size of the process.", h.resident_bytes());
    return e.text();
}

} // namespace gateway
