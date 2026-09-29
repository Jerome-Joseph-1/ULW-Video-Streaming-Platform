#include "gateway.hpp"

#include "net/socket.hpp"

#include "connection.hpp"

#include <cstdio>
#include <format>

namespace gateway {

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
      views_(deps_.reactor, deps_.views, limits_.view_batch, limits_.view_interval) {}

Gateway::~Gateway() {
    deps_.reactor.cancel_timer(drain_timer_);
}

void Gateway::on_accept(os::UniqueFd conn) noexcept {
    if (draining_) {
        return;
    }
    // A socket that refuses its options is already broken.
    if (!net::tune_connection(conn.get())) {
        ++counters_.connections_rejected;
        return;
    }
    const auto handle = connections_.emplace(*this);
    if (!handle) {
        ++counters_.connections_rejected;
        return;
    }
    Connection* c = connections_.get(*handle);
    auto transport = deps_.transports.attach(std::move(conn), *c);
    if (!transport) {
        ++counters_.connections_rejected;
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
        return;
    }
    ++counters_.certificate_reload_failures;
    static_cast<void>(
        std::fputs("gateway_server: certificate reload failed, keeping the old one: ", stderr));
    static_cast<void>(std::fputs(result.error().c_str(), stderr));
    static_cast<void>(std::fputc('\n', stderr));
}

void Gateway::begin_drain() noexcept {
    if (draining_) {
        return;
    }
    // /readyz answers 503 from here on, so the load balancer stops sending new work while
    // requests in flight finish.
    draining_ = true;
    deps_.reactor.stop_listening();
    connections_.for_each_live([](Connection& c) { c.drain(); });
    views_.drain();
    drain_timer_ = deps_.reactor.arm_timer(limits_.drain_deadline, *this);
}

// Whatever is still running now is cut off: the process is about to be killed anyway, and a
// request the client can retry is better ended by us than by SIGKILL.
void Gateway::on_timeout() noexcept {
    drain_timer_ = {};
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

std::string Gateway::render_metrics() const {
    const Counters& c = counters_;
    const ViewCounters& v = views_.counters();
    return std::format(
        "requests_total {}\n"
        "connections_accepted_total {}\n"
        "connections_rejected_total{{reason=\"capacity\"}} {}\n"
        "connections_current {}\n"
        "uploads_in_flight {}\n"
        "admission_rejections_total {}\n"
        "bytes_ingested_total {}\n"
        "timeouts_total{{kind=\"header\"}} {}\n"
        "timeouts_total{{kind=\"body\"}} {}\n"
        "timeouts_total{{kind=\"body_rate\"}} {}\n"
        "timeouts_total{{kind=\"backend\"}} {}\n"
        "timeouts_total{{kind=\"backstop\"}} {}\n"
        "tls_handshakes_in_flight {}\n"
        "tls_handshake_failures_total {}\n"
        "certificate_reloads_total {}\n"
        "certificate_reload_failures_total {}\n"
        "playlist_requests_total{{kind=\"master\"}} {}\n"
        "playlist_requests_total{{kind=\"media\"}} {}\n"
        "playlists_rejected_total {}\n"
        "presign_failures_total {}\n"
        "view_events_recorded_total {}\n"
        "view_events_dropped_total {}\n"
        "view_batches_failed_total {}\n",
        c.requests, c.connections_accepted, c.connections_rejected, connections_.size(),
        upload_slots_, c.admission_rejections, c.bytes_ingested, c.timeouts_header, c.timeouts_body,
        c.timeouts_body_rate, c.timeouts_backend, c.timeouts_backstop,
        deps_.transports.handshakes_in_flight(), deps_.transports.handshake_failures(),
        c.certificate_reloads, c.certificate_reload_failures, c.playlists_master, c.playlists_media,
        c.playlists_rejected, c.presign_failures, v.recorded, v.dropped, v.failed_batches);
}

} // namespace gateway
