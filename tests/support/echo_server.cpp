#include "echo_server.hpp"

#include "net/socket.hpp"

namespace ulw::test {

class EchoServer::Session final : public net::IStreamHandler, public net::ITimerHandler {
public:
    using Handle = net::Slab<Session>::Handle;

    Session(Handle handle, EchoServer& server) noexcept : handle_(handle), server_(server) {}
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    ~Session() override = default;

    void start(net::ConnId conn) noexcept {
        conn_ = conn;
        last_activity_ = server_.reactor_.now();
        timer_ = server_.reactor_.arm_timer(server_.options_.idle_timeout, *this);
        server_.reactor_.start_receiving(conn_);
    }

    [[nodiscard]] net::ConnId conn() const noexcept { return conn_; }

    void on_data(net::BorrowedBytes bytes) noexcept override {
        net::IReactor& r = server_.reactor_;
        last_activity_ = r.now();
        r.send(conn_, bytes);
        if (r.pending_send_bytes(conn_) > server_.options_.high_watermark) {
            throttled_ = true;
            r.stop_receiving(conn_);
        }
    }

    void on_writable() noexcept override {
        net::IReactor& r = server_.reactor_;
        last_activity_ = r.now();
        if (eof_ || server_.draining_) {
            close();
            return;
        }
        if (throttled_) {
            throttled_ = false;
            r.start_receiving(conn_);
        }
    }

    void on_peer_eof() noexcept override {
        eof_ = true;
        if (server_.reactor_.pending_send_bytes(conn_) == 0) {
            close();
        }
    }

    void on_error(int /*err*/) noexcept override { close(); }

    // One sliding timer per session: activity only records a timestamp, and the timer
    // re-arms itself for whatever remains when it fires early.
    void on_timeout() noexcept override {
        timer_ = {};
        const core::Millis idle =
            std::chrono::duration_cast<core::Millis>(server_.reactor_.now() - last_activity_);
        if (idle >= server_.options_.idle_timeout) {
            close();
            return;
        }
        timer_ = server_.reactor_.arm_timer(server_.options_.idle_timeout - idle, *this);
    }

    void drain() noexcept {
        if (server_.reactor_.pending_send_bytes(conn_) == 0) {
            close();
        } else {
            server_.reactor_.stop_receiving(conn_);
        }
    }

    void close() noexcept {
        if (closed_) {
            return;
        }
        closed_ = true;
        server_.reactor_.cancel_timer(timer_);
        server_.reactor_.begin_close(conn_);
        server_.retire(handle_);
    }

private:
    Handle handle_;
    EchoServer& server_;
    net::ConnId conn_;
    net::TimerId timer_;
    core::MonoTime last_activity_;
    bool throttled_ = false;
    bool eof_ = false;
    bool closed_ = false;
};

EchoServer::EchoServer(net::IReactor& reactor, EchoOptions options)
    : reactor_(reactor), options_(options), sessions_(options.max_connections) {}

EchoServer::~EchoServer() {
    reactor_.cancel_timer(drain_timer_);
    sessions_.for_each_live([](Session& s) { s.close(); });
    reap();
}

void EchoServer::on_accept(os::UniqueFd conn) noexcept {
    if (draining_) {
        return;
    }
    // Tuning only trims latency; an untuned connection still echoes correctly.
    [[maybe_unused]] const auto tuned = net::tune_connection(conn.get());
    const auto handle = sessions_.emplace(*this);
    if (!handle) {
        ++rejected_;
        return;
    }
    Session* s = sessions_.get(*handle);
    auto id = reactor_.attach(std::move(conn), *s);
    if (!id) {
        ++rejected_;
        sessions_.retire(*handle);
        return;
    }
    s->start(*id);
}

void EchoServer::on_signal(net::Signal signal) noexcept {
    switch (signal) {
    case net::Signal::Terminate:
        begin_drain();
        return;
    case net::Signal::Reload:
        return;
    }
}

void EchoServer::begin_drain() noexcept {
    if (draining_) {
        return;
    }
    draining_ = true;
    reactor_.stop_listening();
    sessions_.for_each_live([](Session& s) { s.drain(); });
    drain_timer_ = reactor_.arm_timer(options_.drain_deadline, *this);
}

void EchoServer::on_timeout() noexcept {
    drain_timer_ = {};
    sessions_.for_each_live([](Session& s) { s.close(); });
}

void EchoServer::retire(net::Slab<Session>::Handle handle) noexcept {
    sessions_.retire(handle);
}

void EchoServer::reap() noexcept {
    sessions_.reap([this](Session& s) { return reactor_.is_quiescent(s.conn()); });
    if (finished()) {
        reactor_.cancel_timer(drain_timer_);
    }
}

} // namespace ulw::test
