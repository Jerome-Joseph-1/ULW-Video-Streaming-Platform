#include "webhook_server.hpp"

#include "http/request.hpp"
#include "http/request_parser.hpp"
#include "http/response.hpp"

#include <algorithm>
#include <chrono>
#include <optional>
#include <span>
#include <string>
#include <utility>

namespace gateway {

namespace {

using http::Status;

// A refused request's answer has gone out and the write side is shut; the peer gets this long
// to read it and close before the socket goes.
constexpr core::Millis kLinger{1'000};
constexpr std::size_t kResponseHead = 512;

} // namespace

class WebhookServer::Connection final : public net::IStreamHandler,
                                        public http::IRequestSink,
                                        public net::ITimerHandler {
public:
    explicit Connection(WebhookServer& server) noexcept : server_(server), parser_(*this) {}
    ~Connection() override { cancel_timer(); }
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    [[nodiscard]] bool attach(os::UniqueFd fd) noexcept {
        auto id = server_.reactor_.attach(std::move(fd), *this);
        if (!id) {
            closed_ = true;
            return false;
        }
        conn_ = *id;
        attached_ = true;
        server_.reactor_.start_receiving(conn_);
        arm(server_.limits_.idle_timeout);
        return true;
    }

    [[nodiscard]] bool released() const noexcept {
        return closed_ && (!attached_ || server_.reactor_.is_quiescent(conn_));
    }

    void close() noexcept {
        if (closed_) {
            return;
        }
        closed_ = true;
        cancel_timer();
        server_.reactor_.begin_close(conn_);
    }

    void on_data(net::BorrowedBytes bytes) noexcept override {
        if (closed_ || finishing_) {
            return;
        }
        const core::MonoTime now = server_.reactor_.now();
        if (!deadline_) {
            deadline_ = now + server_.limits_.request_timeout;
        }
        const auto left = std::chrono::duration_cast<core::Millis>(*deadline_ - now);
        arm(std::max(core::Millis{0}, std::min(server_.limits_.idle_timeout, left)));
        handle(parser_.feed(bytes));
    }

    void on_writable() noexcept override {}
    void on_peer_eof() noexcept override { close(); }
    void on_error(int /*err*/) noexcept override { close(); }

    void on_timeout() noexcept override {
        timer_.reset();
        close();
    }

    http::HeadVerdict on_head(const http::RequestHead& head) noexcept override {
        body_.clear();
        authorization_.clear();
        too_large_ = false;
        retry_after_.reset();
        keep_alive_ = head.keep_alive;
        WebhookCounters& c = server_.counters_;
        ++c.requests;
        if (head.target != kWebhookPath) {
            ++c.bad_requests;
            return http::HeadVerdict::reject(Status::NotFound);
        }
        if (head.method != http::Method::Post) {
            ++c.bad_requests;
            return http::HeadVerdict::reject(Status::MethodNotAllowed);
        }
        if (head.content_length > kMaxWebhookBody) {
            ++c.refused.at(static_cast<std::size_t>(WebhookRejection::TooLarge));
            return http::HeadVerdict::reject(Status::ContentTooLarge);
        }
        const auto taken = server_.bucket_.take(server_.limits_.rate, server_.reactor_.now());
        if (!taken) {
            ++c.limited;
            retry_after_ = retry_after(taken.error());
            return http::HeadVerdict::reject(Status::TooManyRequests);
        }
        authorization_ = http::find_header(head.headers, "authorization").value_or("");
        return http::HeadVerdict::accept();
    }

    http::BodyVerdict on_body(std::span<const std::byte> bytes) noexcept override {
        if (too_large_ || body_.size() + bytes.size() > kMaxWebhookBody) {
            too_large_ = true;
            return http::BodyVerdict::Continue;
        }
        std::ranges::transform(bytes, std::back_inserter(body_),
                               [](std::byte b) { return static_cast<char>(b); });
        return http::BodyVerdict::Continue;
    }

    void on_message_complete() noexcept override {}

private:
    void handle(http::ParseResult result) noexcept {
        for (;;) {
            if (!result) {
                if (result.error().status != Status::TooManyRequests &&
                    result.error().status != Status::ContentTooLarge &&
                    result.error().status != Status::NotFound &&
                    result.error().status != Status::MethodNotAllowed) {
                    ++server_.counters_.bad_requests;
                }
                refuse(result.error().status);
                return;
            }
            switch (*result) {
            case http::ParseProgress::NeedMore:
            case http::ParseProgress::Paused:
                return;
            case http::ParseProgress::MessageComplete:
                break;
            }
            if (!answer()) {
                return;
            }
            // The next request's time counts from its own first byte.
            deadline_.reset();
            arm(server_.limits_.idle_timeout);
            parser_.reset_for_next_request();
            result = parser_.resume();
        }
    }

    // True when the connection carries on to its next request.
    bool answer() noexcept {
        WebhookCounters& c = server_.counters_;
        if (too_large_) {
            ++c.refused.at(static_cast<std::size_t>(WebhookRejection::TooLarge));
            refuse(Status::ContentTooLarge);
            return false;
        }
        const auto verified =
            verify_webhook(authorization_, body_, server_.key_, server_.clock_.wall_now());
        if (!verified) {
            ++c.refused.at(static_cast<std::size_t>(verified.error()));
            server_.log_.warn("live webhook refused", {{"reason", to_string(verified.error())}});
            refuse(Status::Unauthorized);
            return false;
        }
        const auto event = parse_webhook_event(body_);
        if (!event) {
            ++c.bad_requests;
            refuse(Status::BadRequest);
            return false;
        }
        ++c.accepted;
        server_.log_.debug("live webhook", {{"event", to_string(event->kind)},
                                            {"id", event->id},
                                            {"room", event->room},
                                            {"participant", event->identity}});
        const bool keep = keep_alive_;
        respond(Status::Ok, keep ? http::Connection::KeepAlive : http::Connection::Close);
        // Answered first: LiveKit's next event for the room waits on this answer, not on what
        // the stream service makes of this one.
        server_.sink_.on_event(*event);
        if (!keep) {
            linger();
            return false;
        }
        return true;
    }

    void refuse(Status status) noexcept {
        respond(status, http::Connection::Close);
        linger();
    }

    void linger() noexcept {
        finishing_ = true;
        server_.reactor_.shutdown_write(conn_);
        arm(kLinger);
    }

    void respond(Status status, http::Connection connection) noexcept {
        std::array<char, kResponseHead> buf{};
        const auto n = http::write_response_head({.status = status,
                                                  .content_length = 0,
                                                  .connection = connection,
                                                  .retry_after = retry_after_},
                                                 buf);
        const std::string_view head =
            n ? std::string_view(buf.data(), *n)
              : http::fixed_response(Status::InternalServerError, http::Connection::Close);
        server_.reactor_.send(conn_, std::as_bytes(std::span(head)));
    }

    void arm(core::Millis delay) noexcept {
        cancel_timer();
        timer_ = server_.reactor_.arm_timer(delay, *this);
    }

    void cancel_timer() noexcept {
        if (timer_) {
            server_.reactor_.cancel_timer(*timer_);
            timer_.reset();
        }
    }

    WebhookServer& server_;
    http::RequestParser parser_;
    net::ConnId conn_;
    std::optional<net::TimerId> timer_;
    std::string body_;
    std::string authorization_;
    std::optional<std::chrono::seconds> retry_after_;
    // When the request under way must be in.
    std::optional<core::MonoTime> deadline_;
    bool keep_alive_ = true;
    bool too_large_ = false;
    bool finishing_ = false;
    bool attached_ = false;
    bool closed_ = false;
};

WebhookServer::WebhookServer(net::IReactor& reactor, const core::ports::IClock& clock,
                             WebhookKey key, IWebhookSink& sink, ops::Logger& log,
                             WebhookLimits limits)
    : reactor_(reactor), clock_(clock), key_(std::move(key)), sink_(sink), log_(log),
      limits_(limits), bucket_(limits.rate, reactor.now()) {}

WebhookServer::~WebhookServer() {
    for (const auto& c : connections_) {
        c->close();
    }
}

void WebhookServer::on_accept(os::UniqueFd conn) noexcept {
    reap();
    if (connections_.size() >= limits_.max_connections) {
        ++counters_.refused_connections;
        return;
    }
    auto made = std::make_unique<Connection>(*this);
    if (!made->attach(std::move(conn))) {
        ++counters_.refused_connections;
        return;
    }
    ++counters_.connections;
    connections_.push_back(std::move(made));
}

void WebhookServer::reap() noexcept {
    std::erase_if(connections_, [](const std::unique_ptr<Connection>& c) { return c->released(); });
}

} // namespace gateway
