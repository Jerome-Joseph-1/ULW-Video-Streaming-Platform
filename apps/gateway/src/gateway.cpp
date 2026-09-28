#include "gateway.hpp"

#include "net/socket.hpp"

#include "connection.hpp"

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
    : deps_(deps), limits_(std::move(limits)), connections_(limits_.max_connections) {}

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
    auto id = deps_.reactor.attach(std::move(conn), *c);
    if (!id) {
        ++counters_.connections_rejected;
        connections_.retire(*handle);
        return;
    }
    ++counters_.connections_accepted;
    c->start(*id);
}

void Gateway::on_signal(net::Signal signal) noexcept {
    switch (signal) {
    case net::Signal::Terminate:
        begin_drain();
        return;
    case net::Signal::Reload:
        return;
    }
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
    return std::format("requests_total {}\n"
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
                       "timeouts_total{{kind=\"backstop\"}} {}\n",
                       c.requests, c.connections_accepted, c.connections_rejected,
                       connections_.size(), upload_slots_, c.admission_rejections, c.bytes_ingested,
                       c.timeouts_header, c.timeouts_body, c.timeouts_body_rate, c.timeouts_backend,
                       c.timeouts_backstop);
}

} // namespace gateway
