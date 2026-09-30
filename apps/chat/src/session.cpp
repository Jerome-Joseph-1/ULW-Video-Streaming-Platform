#include "session.hpp"

#include "codec/ws/encoder.hpp"
#include "http/response.hpp"
#include "infra/auth/token_extractor.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <span>
#include <utility>

namespace chat {

namespace {

// RFC 6455 section 7.4.1.
constexpr codec::ws::CloseCode kUnsupportedData{1003};
constexpr codec::ws::CloseCode kPolicyViolation{1008};
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
                respond(http::fixed_response(r.error().status, http::Connection::Close));
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
    auth_ = Auth::Passed;
}

void Session::on_keys_refreshed() noexcept {
    if (closed_ || auth_ != Auth::Waiting) {
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
    arm(server_.limits().ping_interval);
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

// Closes on the next iteration. This may be running inside the router's fan-out, which must
// not see its members change under it; until then nothing more is sent or delivered.
void Session::abandon() noexcept {
    phase_ = Phase::Closing;
    arm(core::Millis{0});
}

// ---- lifetime

void Session::on_timeout() noexcept {
    timer_ = {};
    const Limits& limits = server_.limits();
    const auto quiet = std::chrono::duration_cast<core::Millis>(now() - last_heard_);
    // Past a handshake that took too long, a lingering close, or an abandoned connection.
    switch (phase_) {
    case Phase::Request:
    case Phase::Closing:
        close();
        return;
    case Phase::Open:
        if (quiet >= limits.idle_timeout) {
            close();
            return;
        }
        if (quiet < limits.ping_interval) {
            arm(limits.ping_interval - quiet);
            return;
        }
        try {
            send_frame({.opcode = codec::ws::Opcode::Ping,
                        .fin = true,
                        .payload = {},
                        .close_code = codec::ws::CloseCode::NoStatus});
        } catch (const std::bad_alloc&) {
            allocation_failed();
            return;
        }
        // The next check is the idle deadline itself if it comes before another ping would.
        arm(std::min(limits.ping_interval, limits.idle_timeout - quiet));
        return;
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
