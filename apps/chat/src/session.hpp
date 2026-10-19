#pragma once

#include "codec/ws/decoder.hpp"
#include "codec/ws/frame.hpp"
#include "codec/ws/handshake.hpp"
#include "core/ports/auth.hpp"
#include "http/request.hpp"
#include "http/request_parser.hpp"
#include "net/ip_address.hpp"
#include "net/reactor.hpp"
#include "net/slab.hpp"
#include "net/socket.hpp"
#include "rt/room_router.hpp"

#include "call_bell.hpp"
#include "chat.hpp"
#include "chat_service.hpp"
#include "key_directory.hpp"
#include "presence.hpp"

#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace chat {

// One client connection: an HTTP request (a probe, or the upgrade to a WebSocket), then, once
// upgraded and authenticated, JSON commands in text frames, which the chat service carries
// out. Retired through the server, never destroyed from inside its own callbacks.
// Close code for a socket whose token expired: reconnect with a fresh token
// (docs/integration/chat.md). In the range RFC 6455 section 7.4.2 leaves to applications.
inline constexpr codec::ws::CloseCode kTokenExpired{4001};
// With `/rt?refusals=close` (ADR-0116), an upgrade refused for the token, the origin, a limit or
// the key server is accepted and closed at once with one of these, which a browser can read where
// it hides an HTTP status: sign in again (as kTokenExpired), not allowed, too many (the reason
// says when to retry), or the key server is down (retry).
inline constexpr codec::ws::CloseCode kRefusedUnauthorized = kTokenExpired;
inline constexpr codec::ws::CloseCode kRefusedForbidden{4003};
inline constexpr codec::ws::CloseCode kRefusedTooMany{4029};
inline constexpr codec::ws::CloseCode kRefusedUnavailable{4503};

// The device a socket names in its upgrade's query, `/rt?device=<uuid>` (ADR-0111): nullopt when
// it names none; an error when the value is not a UUID or is given twice. Other parameters are
// ignored.
[[nodiscard]] std::expected<std::optional<core::DeviceId>, std::monostate>
device_of(std::string_view target) noexcept;

// Whether the upgrade asks, `/rt?refusals=close`, to be refused on the socket rather than with an
// HTTP status (ADR-0116). Any other value, or none, is the HTTP status, as before.
[[nodiscard]] bool refusals_on_socket(std::string_view target) noexcept;

// When a socket opened at `now` (monotonic) with a token that expires at `expires_at` stops
// being accepted: exp plus kTokenClockSkew, on the monotonic clock. A token that outlives what
// the monotonic clock can count gets MonoTime::max(); one already past gets `now`.
[[nodiscard]] core::MonoTime token_deadline(core::MonoTime now, core::WallTime wall_now,
                                            core::WallTime expires_at) noexcept;

class Session final : public net::IStreamHandler,
                      public net::ITimerHandler,
                      public http::IRequestSink,
                      public core::ports::IKeyWaiter,
                      public IClient {
public:
    using Handle = net::Slab<Session>::Handle;

    Session(Handle handle, ChatServer& server);
    ~Session() override = default;
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    Session(Session&&) = delete;
    Session& operator=(Session&&) = delete;

    // `hold` counts a direct peer's connection against its address (and IPv6 /48) until the
    // session closes.
    void start(net::ConnId conn, const net::IpAddress& peer,
               std::optional<ChatServer::PeerHold> hold) noexcept;
    [[nodiscard]] net::ConnId conn() const noexcept { return conn_; }

    void on_data(net::BorrowedBytes bytes) noexcept override;
    void on_writable() noexcept override;
    void on_peer_eof() noexcept override;
    void on_error(int err) noexcept override;
    void on_timeout() noexcept override;

    [[nodiscard]] http::HeadVerdict on_head(const http::RequestHead& head) noexcept override;
    [[nodiscard]] http::BodyVerdict on_body(std::span<const std::byte> bytes) noexcept override;
    void on_message_complete() noexcept override;

    void on_keys_refreshed() noexcept override;

    bool push(std::string_view text) noexcept override;
    [[nodiscard]] std::size_t unsent_bytes() const noexcept override;
    void allocation_failed() noexcept override;

    // Close 1001 to an open WebSocket; anything else is closed at once.
    void drain() noexcept;
    void close() noexcept;
    // Whether this session still holds its HTTP parser, which it frees once the request is
    // answered.
    [[nodiscard]] bool parsing_http() const noexcept { return parser_.has_value(); }

private:
    enum class Phase : std::uint8_t { Request, Open, Closing };
    enum class Route : std::uint8_t { Healthz, Readyz, Metrics, Upgrade, NotFound };
    enum class Auth : std::uint8_t { Pending, Waiting, Passed, Refused, KeysDown };

    void route(const http::RequestHead& head) noexcept;
    void authenticate() noexcept;
    void parse_failed(const http::ParseError& error);
    void answer_request();
    void respond(std::string_view bytes);
    void refuse_for_now();
    // ADR-0116: the upgrade accepted, then closed at once with `code` and `reason`.
    void refuse_on_socket(codec::ws::CloseCode code, std::string_view reason);
    void accept_upgrade(const codec::ws::UpgradeResponse& response);
    void leave_http();
    void release_request_hold() noexcept;

    void read_frames(net::BorrowedBytes bytes);
    [[nodiscard]] bool within_control_budget(std::size_t in_this_read) noexcept;
    void command(const codec::ws::Frame& frame);
    void set_visible(bool visible) noexcept;

    void send_text(std::string_view text);
    void send_frame(const codec::ws::Frame& frame);
    // Sends one encoded frame and watches the backlog it leaves.
    void transmit(std::span<const std::byte> frame);
    void close_with(codec::ws::CloseCode code);
    void abandon() noexcept;
    void watch_output() noexcept;
    [[nodiscard]] bool stalled(core::MonoTime at) noexcept;
    [[nodiscard]] std::optional<net::SendProgress> send_progress() const noexcept;
    void give_up() noexcept;
    [[nodiscard]] bool closed_for_expiry(core::MonoTime at) noexcept;
    void arm(core::Millis delay) noexcept;
    [[nodiscard]] core::MonoTime now() const noexcept;

    Handle handle_;
    ChatServer& server_;
    net::ConnId conn_;
    net::TimerId timer_;
    core::MonoTime timer_due_;
    Phase phase_ = Phase::Request;
    core::MonoTime last_heard_;
    core::MonoTime ping_due_;
    // While output waits for the client: how much of it the client had acknowledged when last
    // looked at, and when that last grew (Limits::stall_timeout).
    bool watching_ = false;
    std::uint64_t acked_ = 0;
    core::MonoTime progressed_;

    // Until the request is answered. Its buffers are larger than everything else the session
    // holds while quiet, and after an upgrade it is never needed again.
    std::optional<http::RequestParser> parser_;
    Route route_ = Route::NotFound;
    std::optional<http::Status> refusal_;
    std::optional<std::expected<codec::ws::UpgradeResponse, codec::ws::HandshakeError>> upgrade_;
    // Views the parser's buffer, which is never reset: one request per connection. Cleared
    // with the parser.
    std::string_view token_;
    Auth auth_ = Auth::Pending;
    bool request_complete_ = false;
    bool paused_ = false;
    std::optional<core::UserId> user_;
    // What the upgrade named (device_of), told to the notifier with the user.
    std::optional<core::DeviceId> device_;
    // refusals_on_socket.
    bool socket_refusals_ = false;
    bool notified_ = false;
    // The client said its page or app is out of view (ADR-0125): out of the registry until it
    // says otherwise.
    bool hidden_ = false;
    net::IpAddress peer_;
    // ADR-0076: the direct peer's connection, for the socket's life; a forwarded client's
    // upgrade, until it is answered; and the user's open socket, from the upgrade on.
    std::optional<ChatServer::PeerHold> peer_hold_;
    std::optional<ChatServer::Hold> request_hold_;
    std::optional<ChatServer::Hold> user_hold_;
    // Seconds to put in a 429's Retry-After.
    std::chrono::seconds retry_after_{1};
    // When the token the socket was opened with stops being accepted: its exp plus the clock
    // skew any check allows. The socket is closed then, with kTokenExpired (ADR-0073).
    std::optional<core::MonoTime> expires_;
    // Set by the upgrade.
    std::optional<ClientId> client_;
    std::optional<PresenceClientId> presence_;
    std::optional<BellId> bell_;
    std::optional<DirectoryClientId> directory_;

    codec::ws::Decoder decoder_;
    std::uint32_t control_tokens_;
    core::MonoTime control_refilled_;
    bool closed_ = false;
};

} // namespace chat
