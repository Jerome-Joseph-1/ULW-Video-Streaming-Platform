#pragma once

#include "codec/ws/decoder.hpp"
#include "codec/ws/frame.hpp"
#include "codec/ws/handshake.hpp"
#include "core/ports/auth.hpp"
#include "http/request.hpp"
#include "http/request_parser.hpp"
#include "net/reactor.hpp"
#include "net/slab.hpp"
#include "rt/room_router.hpp"

#include "chat.hpp"
#include "chat_service.hpp"

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace chat {

// One client connection: an HTTP request (a probe, or the upgrade to a WebSocket), then, once
// upgraded and authenticated, JSON commands in text frames, which the chat service carries
// out. Retired through the server, never destroyed from inside its own callbacks.
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

    void start(net::ConnId conn) noexcept;
    [[nodiscard]] net::ConnId conn() const noexcept { return conn_; }

    void on_data(net::BorrowedBytes bytes) noexcept override;
    void on_writable() noexcept override {}
    void on_peer_eof() noexcept override;
    void on_error(int err) noexcept override;
    void on_timeout() noexcept override;

    [[nodiscard]] http::HeadVerdict on_head(const http::RequestHead& head) noexcept override;
    [[nodiscard]] http::BodyVerdict on_body(std::span<const std::byte> bytes) noexcept override;
    void on_message_complete() noexcept override;

    void on_keys_refreshed() noexcept override;

    void push(std::string_view text) noexcept override;
    [[nodiscard]] std::size_t unsent_bytes() const noexcept override;
    void allocation_failed() noexcept override;

    // Close 1001 to an open WebSocket; anything else is closed at once.
    void drain() noexcept;
    void close() noexcept;

private:
    enum class Phase : std::uint8_t { Request, Open, Closing };
    enum class Route : std::uint8_t { Healthz, Readyz, Metrics, Upgrade, NotFound };
    enum class Auth : std::uint8_t { Pending, Waiting, Passed, Refused, KeysDown };

    void route(const http::RequestHead& head) noexcept;
    void authenticate() noexcept;
    void answer_request();
    void respond(std::string_view bytes);
    void accept_upgrade(const codec::ws::UpgradeResponse& response);

    void read_frames(net::BorrowedBytes bytes);
    [[nodiscard]] bool within_control_budget(std::size_t in_this_read) noexcept;
    void command(const codec::ws::Frame& frame);

    void send_text(const std::string& text);
    void send_frame(const codec::ws::Frame& frame);
    void close_with(codec::ws::CloseCode code);
    void abandon() noexcept;
    void arm(core::Millis delay) noexcept;
    [[nodiscard]] core::MonoTime now() const noexcept;

    Handle handle_;
    ChatServer& server_;
    net::ConnId conn_;
    net::TimerId timer_;
    Phase phase_ = Phase::Request;
    core::MonoTime last_heard_;

    http::RequestParser parser_;
    Route route_ = Route::NotFound;
    std::optional<http::Status> refusal_;
    std::optional<std::expected<codec::ws::UpgradeResponse, codec::ws::HandshakeError>> upgrade_;
    // Views the parser's buffer, which is never reset: one request per connection.
    std::string_view token_;
    Auth auth_ = Auth::Pending;
    bool request_complete_ = false;
    bool paused_ = false;
    std::optional<core::UserId> user_;
    // Set by the upgrade.
    std::optional<ClientId> client_;

    codec::ws::Decoder decoder_;
    std::uint32_t control_tokens_;
    core::MonoTime control_refilled_;
    bool closed_ = false;
};

} // namespace chat
