#include "ws_echo_server.hpp"

#include "codec/ws/decoder.hpp"
#include "codec/ws/encoder.hpp"
#include "codec/ws/frame.hpp"
#include "codec/ws/handshake.hpp"
#include "http/request.hpp"
#include "http/request_parser.hpp"
#include "http/response.hpp"
#include "net/socket.hpp"

#include <algorithm>
#include <cstddef>
#include <expected>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace ulw::test {
namespace {

std::span<const std::byte> bytes_of(std::string_view text) noexcept {
    return std::as_bytes(std::span{text});
}

} // namespace

class WsEchoServer::Session final : public net::IStreamHandler,
                                    public net::ITimerHandler,
                                    public http::IRequestSink {
public:
    using Handle = net::Slab<Session>::Handle;

    Session(Handle handle, WsEchoServer& server)
        : handle_(handle), server_(server), parser_(*this),
          decoder_(server.options_.max_message_bytes) {}
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    Session(Session&&) = delete;
    Session& operator=(Session&&) = delete;
    ~Session() override = default;

    void start(net::ConnId conn) noexcept {
        conn_ = conn;
        last_activity_ = server_.reactor_.now();
        timer_ = server_.reactor_.arm_timer(server_.options_.idle_timeout, *this);
        server_.reactor_.start_receiving(conn_);
    }

    [[nodiscard]] net::ConnId conn() const noexcept { return conn_; }

    void on_data(net::BorrowedBytes bytes) noexcept override {
        last_activity_ = server_.reactor_.now();
        switch (phase_) {
        case Phase::Handshake:
            handshake(bytes);
            break;
        case Phase::Open:
            echo(bytes);
            break;
        case Phase::Closing:
            break;
        }
        flush();
    }

    void on_writable() noexcept override {
        last_activity_ = server_.reactor_.now();
        flush();
        if (eof_ && idle()) {
            close();
        }
    }

    void on_peer_eof() noexcept override {
        eof_ = true;
        if (idle()) {
            close();
        }
    }

    void on_error(int /*err*/) noexcept override { close(); }

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

    http::HeadVerdict on_head(const http::RequestHead& head) noexcept override {
        // Kept rather than acted on: the response goes out once the request has ended.
        upgrade_ = codec::ws::accept_handshake(head);
        return http::HeadVerdict::accept();
    }

    http::BodyVerdict on_body(std::span<const std::byte> /*bytes*/) noexcept override {
        return http::BodyVerdict::Continue;
    }

    void on_message_complete() noexcept override {}

    void drain() noexcept {
        if (phase_ != Phase::Open) {
            close();
            return;
        }
        send_close(codec::ws::CloseCode::GoingAway);
        flush();
    }

    void close() noexcept {
        if (closed_) {
            return;
        }
        closed_ = true;
        server_.reactor_.cancel_timer(timer_);
        server_.reactor_.begin_close(conn_);
        server_.sessions_.retire(handle_);
    }

private:
    enum class Phase : std::uint8_t { Handshake, Open, Closing };

    // A client sends nothing after its opening handshake until it has read the 101 (RFC 6455
    // section 4.1), so no frame can arrive in the same read as the request.
    void handshake(net::BorrowedBytes bytes) noexcept {
        const http::ParseResult r = parser_.feed(bytes);
        if (!r) {
            queue(bytes_of(http::fixed_response(r.error().status, http::Connection::Close)));
            hang_up();
            return;
        }
        if (*r != http::ParseProgress::MessageComplete || !upgrade_) {
            return;
        }
        if (!*upgrade_) {
            queue(bytes_of(codec::ws::rejection_response(upgrade_->error())));
            hang_up();
            return;
        }
        queue(bytes_of((*upgrade_)->bytes()));
        upgrade_.reset();
        phase_ = Phase::Open;
    }

    // feed() and encode() allocate, so a failed allocation here terminates the process: fine for
    // a test server, a decision still to make for the chat server (ADR-0029).
    void echo(net::BorrowedBytes bytes) noexcept {
        codec::ws::Decoded decoded = decoder_.feed(bytes);
        for (codec::ws::Frame& frame : decoded.frames) {
            switch (frame.opcode) {
            case codec::ws::Opcode::Text:
            case codec::ws::Opcode::Binary:
                send_frame(frame);
                break;
            case codec::ws::Opcode::Ping:
                frame.opcode = codec::ws::Opcode::Pong;
                send_frame(frame);
                break;
            case codec::ws::Opcode::Close:
                send_close(frame.close_code);
                return;
            case codec::ws::Opcode::Pong:
            case codec::ws::Opcode::Continuation:
                break;
            }
        }
        if (decoded.error) {
            send_close(*decoded.error);
        }
    }

    void send_frame(const codec::ws::Frame& frame) noexcept {
        if (!codec::ws::encode(frame, out_)) {
            close();
        }
    }

    // A Close without a status is answered with one without a status (RFC 6455 section 5.5.1).
    void send_close(codec::ws::CloseCode code) noexcept {
        send_frame(
            {.opcode = codec::ws::Opcode::Close, .fin = true, .payload = {}, .close_code = code});
        hang_up();
    }

    void queue(std::span<const std::byte> bytes) {
        out_.insert(out_.end(), bytes.begin(), bytes.end());
    }

    // FIN follows once everything queued is sent; whatever arrives meanwhile is read and dropped
    // until the peer's own FIN.
    void hang_up() noexcept { phase_ = Phase::Closing; }

    [[nodiscard]] bool idle() const noexcept {
        return out_.empty() && server_.reactor_.pending_send_bytes(conn_) == 0;
    }

    // The reactor refuses a send that would take its queue past a bound (4 MiB), and Autobahn's
    // messages go to 16 MiB, so an echo goes out in pieces as the queue drains. Reading stops
    // while any of it waits, which bounds out_ to about one message.
    void flush() noexcept {
        if (closed_) {
            return;
        }
        net::IReactor& r = server_.reactor_;
        const std::size_t watermark = server_.options_.high_watermark;
        while (sent_ < out_.size() && r.pending_send_bytes(conn_) < watermark) {
            const std::size_t n = std::min(kSendPiece, out_.size() - sent_);
            r.send(conn_, std::span{out_}.subspan(sent_, n));
            sent_ += n;
        }
        if (sent_ == out_.size()) {
            out_.clear();
            sent_ = 0;
            if (phase_ == Phase::Closing && !fin_sent_) {
                fin_sent_ = true;
                r.shutdown_write(conn_);
            }
        }
        const bool backed_up = !out_.empty() || r.pending_send_bytes(conn_) > watermark;
        if (backed_up != throttled_) {
            throttled_ = backed_up;
            if (backed_up) {
                r.stop_receiving(conn_);
            } else {
                r.start_receiving(conn_);
            }
        }
    }

    // One receive buffer's worth.
    static constexpr std::size_t kSendPiece = std::size_t{64} * 1024;

    Handle handle_;
    WsEchoServer& server_;
    net::ConnId conn_;
    net::TimerId timer_;
    core::MonoTime last_activity_;
    Phase phase_ = Phase::Handshake;
    http::RequestParser parser_;
    std::optional<std::expected<codec::ws::UpgradeResponse, codec::ws::HandshakeError>> upgrade_;
    codec::ws::Decoder decoder_;
    std::vector<std::byte> out_;
    std::size_t sent_ = 0;
    bool fin_sent_ = false;
    bool throttled_ = false;
    bool eof_ = false;
    bool closed_ = false;
};

WsEchoServer::WsEchoServer(net::IReactor& reactor, WsEchoOptions options)
    : reactor_(reactor), options_(options), sessions_(options.max_connections) {}

WsEchoServer::~WsEchoServer() {
    reactor_.cancel_timer(drain_timer_);
    sessions_.for_each_live([](Session& s) { s.close(); });
    reap();
}

void WsEchoServer::on_accept(os::UniqueFd conn) noexcept {
    if (draining_) {
        return;
    }
    [[maybe_unused]] const auto tuned = net::tune_connection(conn.get());
    const auto handle = sessions_.emplace(*this);
    if (!handle) {
        return;
    }
    Session* s = sessions_.get(*handle);
    auto id = reactor_.attach(std::move(conn), *s);
    if (!id) {
        sessions_.retire(*handle);
        return;
    }
    s->start(*id);
}

void WsEchoServer::on_signal(net::Signal signal) noexcept {
    switch (signal) {
    case net::Signal::Terminate:
        begin_drain();
        return;
    case net::Signal::Reload:
        return;
    }
}

void WsEchoServer::begin_drain() noexcept {
    if (draining_) {
        return;
    }
    draining_ = true;
    reactor_.stop_listening();
    sessions_.for_each_live([](Session& s) { s.drain(); });
    drain_timer_ = reactor_.arm_timer(options_.drain_deadline, *this);
}

void WsEchoServer::on_timeout() noexcept {
    drain_timer_ = {};
    sessions_.for_each_live([](Session& s) { s.close(); });
}

void WsEchoServer::reap() noexcept {
    sessions_.reap([this](Session& s) { return reactor_.is_quiescent(s.conn()); });
    if (finished()) {
        reactor_.cancel_timer(drain_timer_);
    }
}

} // namespace ulw::test
