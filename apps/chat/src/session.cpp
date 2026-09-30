#include "session.hpp"

#include "codec/ws/encoder.hpp"
#include "http/response.hpp"
#include "infra/auth/token_extractor.hpp"
#include "net/socket.hpp"

#include "log.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <span>
#include <utility>

namespace chat {

namespace {

// RFC 6455 section 7.4.1.
constexpr codec::ws::CloseCode kUnsupportedData{1003};
constexpr codec::ws::CloseCode kPolicyViolation{1008};
constexpr codec::ws::CloseCode kMessageTooBig{1009};
constexpr codec::ws::CloseCode kInternalError{1011};

std::span<const std::byte> bytes_of(std::string_view text) noexcept {
    return std::as_bytes(std::span{text});
}

std::string_view text_of(std::span<const std::byte> bytes) noexcept {
    // The bytes are characters; reading them as such is what a text frame means.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

} // namespace

Session::Session(Handle handle, ChatServer& server)
    : handle_(handle), server_(server), parser_(std::in_place, *this),
      control_tokens_(server.limits().control_burst) {}

core::MonoTime Session::now() const noexcept {
    return server_.deps().reactor.now();
}

void Session::start(net::ConnId conn) noexcept {
    conn_ = conn;
    last_heard_ = control_refilled_ = now();
    arm(server_.limits().handshake_timeout);
    server_.deps().reactor.start_receiving(conn_);
}

void Session::arm(core::Millis delay) noexcept {
    net::IReactor& reactor = server_.deps().reactor;
    reactor.cancel_timer(timer_);
    timer_ = reactor.arm_timer(delay, *this);
    timer_due_ = now() + delay;
}

// ---- the request

void Session::on_data(net::BorrowedBytes bytes) noexcept {
    last_heard_ = now();
    try {
        switch (phase_) {
        case Phase::Request: {
            if (!parser_) {
                return;
            }
            const http::ParseResult r = parser_->feed(bytes);
            if (!r) {
                parse_failed(r.error());
            }
            leave_http();
            return;
        }
        case Phase::Open:
            read_frames(bytes);
            return;
        case Phase::Closing:
            return;
        }
    } catch (const std::bad_alloc&) {
        allocation_failed();
    }
}

http::HeadVerdict Session::on_head(const http::RequestHead& head) noexcept {
    // Nothing here takes a body; a GET that announces one is not one of ours.
    if (head.content_length != 0) {
        return http::HeadVerdict::reject(http::Status::BadRequest);
    }
    route(head);
    return http::HeadVerdict::accept();
}

void Session::route(const http::RequestHead& head) noexcept {
    if (head.method != http::Method::Get) {
        route_ = Route::NotFound;
        return;
    }
    const std::string_view path = head.target.substr(0, head.target.find('?'));
    if (path == "/healthz") {
        route_ = Route::Healthz;
        return;
    }
    if (path == "/readyz") {
        route_ = Route::Readyz;
        return;
    }
    if (path == "/metrics") {
        route_ = Route::Metrics;
        return;
    }
    if (path != "/rt") {
        route_ = Route::NotFound;
        return;
    }
    route_ = Route::Upgrade;
    upgrade_ = codec::ws::accept_handshake(head);
    if (!*upgrade_) {
        return;
    }
    const Access& access = server_.access();
    infra::auth::TokenExtractor extractor(access.cookie);
    for (const http::HeaderField& h : head.headers) {
        extractor.on_header(h.name, h.value);
    }
    const auto token = extractor.token();
    if (!token) {
        refusal_ = http::Status::Unauthorized;
        return;
    }
    // A browser attaches the cookie to a socket any page opens (RFC 6455 section 10.2), so a
    // cookie counts only from an allowed page. A browser cannot set Authorization on a socket,
    // so a bearer token came from a client that chose to send it.
    if (!http::find_header(head.headers, "authorization")) {
        const auto origin = http::find_header(head.headers, "origin");
        if (!origin ||
            std::ranges::find(access.allowed_origins, *origin) == access.allowed_origins.end()) {
            ++server_.counters().origin_rejections;
            refusal_ = http::Status::Forbidden;
            return;
        }
    }
    token_ = *token;
    authenticate();
}

core::MonoTime token_deadline(core::MonoTime now, core::WallTime wall_now,
                              core::WallTime expires_at) noexcept {
    // An exp may be anything up to the wall clock's last second but one, so the skew is added to
    // what is left, not to exp, and the sum held to what the clocks can count.
    using Duration = core::WallTime::duration;
    const Duration skew = core::ports::kTokenClockSkew;
    Duration left = expires_at - wall_now;
    // Only a wall clock within a minute of the epoch leaves room for this to saturate, since exp
    // is at most the clock's last second but one; kept so that no clock reading can overflow.
    left = left > Duration::max() - skew ? Duration::max() : left + skew;
    if (left <= Duration::zero()) {
        return now;
    }
    if (left >= core::MonoTime::max() - now) {
        return core::MonoTime::max();
    }
    return now + std::chrono::duration_cast<core::MonoTime::duration>(left);
}

void Session::authenticate() noexcept {
    const auto result =
        server_.deps().verifier.verify(token_, server_.deps().clock.wall_now(), *this);
    if (!result) {
        auth_ = Auth::Waiting;
        return;
    }
    if (!*result) {
        ++server_.counters().auth_failures;
        // An outage at the key server says nothing about the token; a 503 asks the client to
        // retry where a 401 would sign it out.
        auth_ = result->error() == core::ports::AuthError::KeysUnavailable ? Auth::KeysDown
                                                                           : Auth::Refused;
        return;
    }
    user_ = (*result)->subject;
    // On the reactor's clock from here: the wall clock may be stepped while the socket lives.
    expires_ = token_deadline(now(), server_.deps().clock.wall_now(), (*result)->expires_at);
    auth_ = Auth::Passed;
}

void Session::on_keys_refreshed() noexcept {
    // Past the request phase the request has had its answer, an error or an abandonment, and
    // must not get a second one.
    if (closed_ || auth_ != Auth::Waiting || phase_ != Phase::Request) {
        return;
    }
    authenticate();
    if (auth_ == Auth::Waiting || !request_complete_) {
        return;
    }
    try {
        answer_request();
        leave_http();
    } catch (const std::bad_alloc&) {
        allocation_failed();
    }
}

// The parser's one allocation is the buffer for bytes behind a request, and it reports failing to
// grow it as 503. That can happen in the very feed() that accepted an upgrade: the 101 is out, and
// only WebSocket may follow it.
// Not covered by a test: the one way in is std::bad_alloc from growing a std::vector, and injecting
// that means replacing the global operator new in chat_unit_tests, which the ASan and TSan unit
// runs replace themselves.
void Session::parse_failed(const http::ParseError& error) {
    const bool out_of_memory = error.status == http::Status::ServiceUnavailable;
    switch (phase_) {
    case Phase::Request:
        if (out_of_memory) {
            ++server_.counters().allocation_failures;
        }
        respond(http::fixed_response(error.status, http::Connection::Close));
        return;
    case Phase::Open:
        if (out_of_memory) {
            allocation_failed();
            return;
        }
        close_with(kMessageTooBig);
        return;
    case Phase::Closing:
        return;
    }
}

http::BodyVerdict Session::on_body(std::span<const std::byte> /*bytes*/) noexcept {
    return http::BodyVerdict::Continue;
}

void Session::on_message_complete() noexcept {
    request_complete_ = true;
    if (auth_ == Auth::Waiting) {
        // A client sends nothing more until it has the 101 (RFC 6455 section 4.1).
        server_.deps().reactor.stop_receiving(conn_);
        paused_ = true;
        return;
    }
    try {
        answer_request();
    } catch (const std::bad_alloc&) {
        allocation_failed();
    }
}

void Session::answer_request() {
    switch (route_) {
    case Route::Healthz:
        respond(http::fixed_response(http::Status::Ok, http::Connection::Close));
        return;
    case Route::Readyz:
        respond(http::fixed_response(server_.ready() ? http::Status::Ok
                                                     : http::Status::ServiceUnavailable,
                                     http::Connection::Close));
        return;
    case Route::Metrics: {
        const std::string body = server_.render_metrics();
        std::array<char, 256> head{};
        const auto written =
            http::write_response_head({.status = http::Status::Ok,
                                       .content_length = body.size(),
                                       .connection = http::Connection::Close,
                                       .content_type = "text/plain; version=0.0.4"},
                                      head);
        if (!written) {
            respond(
                http::fixed_response(http::Status::InternalServerError, http::Connection::Close));
            return;
        }
        server_.deps().reactor.send(conn_, bytes_of({head.data(), *written}));
        respond(body);
        return;
    }
    case Route::NotFound:
        respond(http::fixed_response(http::Status::NotFound, http::Connection::Close));
        return;
    case Route::Upgrade:
        break;
    }
    if (!upgrade_ || !*upgrade_) {
        const auto error = upgrade_ ? upgrade_->error() : codec::ws::HandshakeError::Malformed;
        respond(codec::ws::rejection_response(error));
        return;
    }
    if (refusal_) {
        respond(http::fixed_response(*refusal_, http::Connection::Close));
        return;
    }
    switch (auth_) {
    case Auth::Passed:
        accept_upgrade(**upgrade_);
        return;
    case Auth::KeysDown:
        respond(http::fixed_response(http::Status::ServiceUnavailable, http::Connection::Close));
        return;
    case Auth::Refused:
    case Auth::Pending:
    case Auth::Waiting:
        respond(http::fixed_response(http::Status::Unauthorized, http::Connection::Close));
        return;
    }
}

// The response, then FIN; the connection closes once the client has closed its side.
void Session::respond(std::string_view bytes) {
    net::IReactor& reactor = server_.deps().reactor;
    reactor.send(conn_, bytes_of(bytes));
    reactor.shutdown_write(conn_);
    phase_ = Phase::Closing;
    arm(server_.limits().handshake_timeout);
}

void Session::accept_upgrade(const codec::ws::UpgradeResponse& response) {
    net::IReactor& reactor = server_.deps().reactor;
    reactor.send(conn_, bytes_of(response.bytes()));
    // Passed authentication, which sets the user; without one, commands close the socket.
    if (user_) {
        client_ = server_.chat().attach(*this, *user_);
        presence_ = server_.presence().attach(*this, *user_);
    }
    phase_ = Phase::Open;
    ++server_.counters().upgrades;
    if (paused_) {
        paused_ = false;
        reactor.start_receiving(conn_);
    }
    core::Millis first = server_.limits().ping_interval;
    if (expires_) {
        first = std::min(first, std::chrono::ceil<core::Millis>(
                                    std::max(*expires_ - now(), core::MonoTime::duration{0})));
    }
    arm(first);
}

// Once the request is answered the connection never parses HTTP again, so the parser goes, with
// its buffers. Called after the parser has returned, never from inside one of its callbacks.
// Whatever the client sent behind an accepted upgrade request is the start of its WebSocket
// stream, and reaches the decoder before anything read later.
void Session::leave_http() {
    if (phase_ == Phase::Request || !parser_) {
        return;
    }
    if (phase_ == Phase::Open) {
        const std::span<const std::byte> pipelined = parser_->unparsed();
        if (!pipelined.empty()) {
            read_frames(pipelined);
        }
    }
    token_ = {};
    parser_.reset();
}

// ---- the WebSocket

void Session::read_frames(net::BorrowedBytes bytes) {
    codec::ws::Decoded decoded = decoder_.feed(bytes);
    std::size_t control = 0;
    for (const codec::ws::Frame& frame : decoded.frames) {
        if (codec::ws::is_control(frame.opcode) && !within_control_budget(++control)) {
            ++server_.counters().control_floods;
            close_with(kPolicyViolation);
            return;
        }
        switch (frame.opcode) {
        case codec::ws::Opcode::Text:
            command(frame);
            break;
        case codec::ws::Opcode::Binary:
            close_with(kUnsupportedData);
            return;
        case codec::ws::Opcode::Ping:
            send_frame({.opcode = codec::ws::Opcode::Pong,
                        .fin = true,
                        .payload = frame.payload,
                        .close_code = codec::ws::CloseCode::NoStatus});
            break;
        case codec::ws::Opcode::Pong:
            break;
        case codec::ws::Opcode::Close:
            close_with(frame.close_code);
            return;
        case codec::ws::Opcode::Continuation:
            break;
        }
        if (phase_ != Phase::Open) {
            return;
        }
    }
    if (decoded.error) {
        ++server_.counters().protocol_errors;
        close_with(*decoded.error);
    }
}

// A token bucket in whole tokens, refilled from the reactor's clock.
bool Session::within_control_budget(std::size_t in_this_read) noexcept {
    const Limits& limits = server_.limits();
    if (in_this_read > limits.max_control_per_read) {
        return false;
    }
    const core::MonoTime t = now();
    const auto elapsed = std::chrono::duration_cast<core::Millis>(t - control_refilled_).count();
    const auto earned = static_cast<std::uint64_t>(elapsed) * limits.control_per_second / 1000;
    if (earned > 0) {
        control_tokens_ = static_cast<std::uint32_t>(
            std::min<std::uint64_t>(limits.control_burst, control_tokens_ + earned));
        control_refilled_ = t;
    }
    if (control_tokens_ == 0) {
        return false;
    }
    --control_tokens_;
    return true;
}

void Session::command(const codec::ws::Frame& frame) {
    ++server_.counters().messages_received;
    auto parsed = parse_command(text_of(frame.payload));
    if (!parsed) {
        std::string out;
        write_error(out, reason(parsed.error()));
        send_text(out);
        return;
    }
    // Set by the upgrade, which is the only way into the Open phase.
    if (!client_ || !presence_) {
        close_with(kInternalError);
        return;
    }
    ChatService& chat = server_.chat();
    if (const auto* j = std::get_if<Join>(&*parsed)) {
        chat.join(*client_, *j);
        return;
    }
    if (const auto* h = std::get_if<History>(&*parsed)) {
        chat.history(*client_, *h);
        return;
    }
    if (auto* s = std::get_if<Send>(&*parsed)) {
        chat.send(*client_, std::move(*s));
        return;
    }
    if (const auto* w = std::get_if<Watch>(&*parsed)) {
        server_.presence().watch(*presence_, w->user);
        return;
    }
    server_.presence().unwatch(*presence_, std::get<Unwatch>(*parsed).user);
}

bool Session::push(std::string_view text) noexcept {
    if (phase_ != Phase::Open) {
        return false;
    }
    try {
        const auto bytes = bytes_of(text);
        send_frame({.opcode = codec::ws::Opcode::Text,
                    .fin = true,
                    .payload = {bytes.begin(), bytes.end()},
                    .close_code = codec::ws::CloseCode::NoStatus});
    } catch (const std::bad_alloc&) {
        allocation_failed();
    }
    // A reader too far behind was abandoned by this very frame, which it will never read.
    return phase_ == Phase::Open;
}

std::size_t Session::unsent_bytes() const noexcept {
    return server_.deps().reactor.pending_send_bytes(conn_);
}

void Session::send_text(const std::string& text) {
    const auto bytes = bytes_of(text);
    send_frame({.opcode = codec::ws::Opcode::Text,
                .fin = true,
                .payload = {bytes.begin(), bytes.end()},
                .close_code = codec::ws::CloseCode::NoStatus});
}

void Session::send_frame(const codec::ws::Frame& frame) {
    if (phase_ != Phase::Open) {
        return;
    }
    std::vector<std::byte> out;
    if (!codec::ws::encode(frame, out)) {
        abandon();
        return;
    }
    net::IReactor& reactor = server_.deps().reactor;
    reactor.send(conn_, out);
    watch_output();
    if (reactor.pending_send_bytes(conn_) > server_.limits().max_backlog) {
        ++server_.counters().slow_consumers;
        abandon();
    }
}

// A Close with `code` (echoing the client's own, or none if it sent none), then FIN.
void Session::close_with(codec::ws::CloseCode code) {
    send_frame(
        {.opcode = codec::ws::Opcode::Close, .fin = true, .payload = {}, .close_code = code});
    if (closed_) {
        return;
    }
    server_.deps().reactor.shutdown_write(conn_);
    phase_ = Phase::Closing;
    arm(server_.limits().handshake_timeout);
}

// ADR-0036: the connection whose input could not be held pays for it, not the process.
void Session::allocation_failed() noexcept {
    ++server_.counters().allocation_failures;
    abandon();
}

// Output now waits for the client: from here it has stall_timeout to acknowledge some of it.
// Looking costs a system call, so it is done on the timer, not for every frame.
void Session::watch_output() noexcept {
    if (watching_) {
        return;
    }
    watching_ = true;
    progressed_ = now();
    // What it had acknowledged when its output began to wait, so that a client that never
    // acknowledges any of it is closed stall_timeout from here, not from a first look.
    if (const auto progress = send_progress()) {
        acked_ = progress->acked;
    }
    const core::Millis check = server_.limits().stall_check;
    if (timer_due_ - progressed_ > check) {
        arm(check);
    }
}

// Whether the client has acknowledged nothing of its waiting output for stall_timeout. Without
// TCP_INFO to tell, it is never counted as stalled; the idle timeout still ends a client that
// answers nothing.
bool Session::stalled(core::MonoTime at) noexcept {
    if (!watching_) {
        return false;
    }
    const auto progress = send_progress();
    if (!progress) {
        watching_ = unsent_bytes() != 0;
        return false;
    }
    if (!progress->waiting && unsent_bytes() == 0) {
        watching_ = false;
        acked_ = progress->acked;
        return false;
    }
    if (progress->acked != acked_) {
        acked_ = progress->acked;
        progressed_ = at;
        return false;
    }
    return at - progressed_ >= server_.limits().stall_timeout;
}

// The connection's TCP_INFO. Where the kernel cannot give it, stalled clients go undetected,
// which the log says once.
std::optional<net::SendProgress> Session::send_progress() const noexcept {
    const auto progress = net::send_progress(conn_.fd);
    if (progress) {
        return *progress;
    }
    static std::atomic_flag told;
    if (!told.test_and_set()) {
        log_event(
            R"("level":"warn","msg":"no TCP_INFO: stalled clients are not closed","errno":{})",
            progress.error());
    }
    return std::nullopt;
}

// A client given up on: the connection is reset, so that the kernel drops what it still holds
// for it instead of keeping it, and a FIN, in an orphan for as long as it probes the peer.
void Session::give_up() noexcept {
    net::abort_on_close(conn_.fd);
    close();
}

// Closes on the next iteration. This may be running inside the router's fan-out, which must
// not see its members change under it; until then nothing more is sent or delivered.
void Session::abandon() noexcept {
    phase_ = Phase::Closing;
    arm(core::Millis{0});
}

// ---- lifetime

// The token the socket was opened with is no longer accepted: the client is to reconnect with
// a fresh one (ADR-0073).
bool Session::closed_for_expiry(core::MonoTime at) noexcept {
    if (!expires_ || at < *expires_) {
        return false;
    }
    ++server_.counters().token_expiries;
    try {
        close_with(kTokenExpired);
    } catch (const std::bad_alloc&) {
        allocation_failed();
    }
    return true;
}

void Session::on_timeout() noexcept {
    timer_ = {};
    const Limits& limits = server_.limits();
    const core::MonoTime at = now();
    const auto quiet = std::chrono::duration_cast<core::Millis>(at - last_heard_);
    // Past a handshake that took too long, a lingering close, or an abandoned connection.
    switch (phase_) {
    case Phase::Request:
    case Phase::Closing:
        close();
        return;
    case Phase::Open: {
        if (closed_for_expiry(at)) {
            return;
        }
        if (quiet >= limits.idle_timeout) {
            give_up();
            return;
        }
        if (stalled(at)) {
            ++server_.counters().stalled_readers;
            give_up();
            return;
        }
        // The next check is the idle deadline itself if it comes before another ping would.
        core::MonoTime next = last_heard_ + limits.idle_timeout;
        if (quiet < limits.ping_interval) {
            next = std::min(next, last_heard_ + limits.ping_interval);
        } else {
            if (at >= ping_due_) {
                try {
                    send_frame({.opcode = codec::ws::Opcode::Ping,
                                .fin = true,
                                .payload = {},
                                .close_code = codec::ws::CloseCode::NoStatus});
                } catch (const std::bad_alloc&) {
                    allocation_failed();
                    return;
                }
                if (phase_ != Phase::Open) {
                    return;
                }
                ping_due_ = at + limits.ping_interval;
            }
            next = std::min(next, ping_due_);
        }
        if (watching_) {
            next = std::min(next, at + limits.stall_check);
        }
        if (expires_) {
            next = std::min(next, *expires_);
        }
        arm(std::chrono::ceil<core::Millis>(next - at));
        return;
    }
    }
}

// A lossy client that fell behind is owed messages until its connection has drained.
void Session::on_writable() noexcept {
    if (phase_ == Phase::Open && client_) {
        server_.chat().drained(*client_);
    }
}

void Session::on_peer_eof() noexcept {
    close();
}

void Session::on_error(int /*err*/) noexcept {
    close();
}

void Session::drain() noexcept {
    if (phase_ != Phase::Open) {
        close();
        return;
    }
    try {
        close_with(codec::ws::CloseCode::GoingAway);
    } catch (const std::bad_alloc&) {
        allocation_failed();
    }
}

void Session::close() noexcept {
    if (closed_) {
        return;
    }
    closed_ = true;
    phase_ = Phase::Closing;
    token_ = {};
    parser_.reset();
    net::IReactor& reactor = server_.deps().reactor;
    reactor.cancel_timer(timer_);
    if (auth_ == Auth::Waiting) {
        server_.deps().verifier.cancel_wait(*this);
    }
    if (client_) {
        server_.chat().detach(*client_);
    }
    if (presence_) {
        server_.presence().detach(*presence_);
    }
    reactor.begin_close(conn_);
    server_.retire(handle_);
}

} // namespace chat
