#include "infra/sfu/livekit/livekit_sfu.hpp"

#include "core/util/json.hpp"

#include "access_token.hpp"
#include "room_service.hpp"

#include <chrono>
#include <cstddef>
#include <string>
#include <utility>

namespace infra::sfu::livekit {

namespace {

using core::ports::IMediaRoom;
using core::ports::MediaDone;
using core::ports::MediaError;
using core::ports::MediaRole;
using core::ports::MediaTicket;
using detail::Grant;
using detail::IfAbsent;
using detail::Permission;
using detail::RoomService;

// A ticket is presented once, to connect: from then on LiveKit keeps the client's token fresh
// itself (at join and every 5 min, pkg/service/roommanager.go in v1.13.7), so reconnects never
// need the ticket again. One connect is the SDK's 15 s signal plus 15 s peer-connection budget,
// and room.connect() retries once: 60 s. Clock skew needs nothing extra, LiveKit allows a minute.
constexpr core::Seconds kTicketTtl{2 * (15 + 15)};
// A WHIP client sends its ticket again with every request on the session (RFC 9725 section 4.5),
// and LiveKit checks it each time (pkg/service/whipservice.go in v1.13.7), so a publisher's
// DELETE at the end of the longest stream the packager takes, 12 h (ADR-0047), must still pass,
// from a session that connected at the end of the ticket's connect window.
constexpr core::Seconds kPublisherTicketTtl = std::chrono::hours{12} + kTicketTtl;
// How long LiveKit keeps a room with nobody in it: since its creation if nobody has joined yet
// (empty_timeout), since the last one left if someone had (departure_timeout). The first must
// cover a ticket issued as the room opens, 60 s. The second must cover the SDK's reconnect after
// everyone's link drops at once: its retry delays add up to 44.1 s (0, 0.3, 1.2, 2.7, 4.8 and
// five of 7 s), which the ticket's 60 s also covers. join re-creates a room dropped anyway.
constexpr core::Seconds kIdleRoomTimeout = kTicketTtl;

// RFC 7518 section 3.2: an HS256 key of at least 256 bits.
constexpr std::size_t kMinSecretBytes = 256 / 8;
// Generated secrets are 43 to 64 characters; 256 bytes is far past any real one.
constexpr std::size_t kMaxSecretBytes = 256;

// Where a client goes for each role: the signalling WebSocket, and the WHIP endpoint LiveKit
// serves on the same port over HTTP.
struct Endpoints {
    std::string client;
    std::string whip;
};

std::string whip_url(std::string_view client_url) {
    const bool secure = client_url.starts_with("wss://");
    std::string url = secure ? "https://" : "http://";
    url += client_url.substr(secure ? std::string_view("wss://").size()
                                    : std::string_view("ws://").size());
    while (url.ends_with('/')) {
        url.pop_back();
    }
    url += "/whip/v1";
    return url;
}

std::string create_room_body(std::string_view name, std::uint16_t max_participants) {
    std::string body = R"({"name":)";
    core::json::append_string(body, name);
    body += R"(,"empty_timeout":)";
    body += std::to_string(kIdleRoomTimeout.count());
    body += R"(,"departure_timeout":)";
    body += std::to_string(kIdleRoomTimeout.count());
    body += R"(,"max_participants":)";
    body += std::to_string(max_participants);
    body += '}';
    return body;
}

// The live stream's single rendition, re-encoded by LiveKit's recorder: the worker's 720p rung
// (core::choose_ladder), at the frame rate browsers capture at.
constexpr int kRelayWidth = 1280;
constexpr int kRelayHeight = 720;
constexpr int kRelayFramerate = 30;
constexpr int kRelayVideoKbps = 2800;

std::string relay_body(std::string_view room, std::string_view identity,
                       const core::ports::MediaRelay& target) {
    std::string body = R"({"room_name":)";
    core::json::append_string(body, room);
    body += R"(,"identity":)";
    core::json::append_string(body, identity);
    body += R"(,"advanced":{"width":)";
    body += std::to_string(kRelayWidth);
    body += R"(,"height":)";
    body += std::to_string(kRelayHeight);
    body += R"(,"framerate":)";
    body += std::to_string(kRelayFramerate);
    body += R"(,"video_bitrate":)";
    body += std::to_string(kRelayVideoKbps);
    body += R"(,"key_frame_interval":)";
    body += std::to_string(target.keyframe_interval.count());
    body += R"(},"stream_outputs":[{"protocol":"SRT","urls":[)";
    core::json::append_string(body, target.url);
    body += "]}]}";
    return body;
}

std::string identity_of(const core::UserId& user, const core::DeviceId& device) {
    // A user id never holds '/', so the identity splits back apart unambiguously.
    std::string identity(user.view());
    identity += '/';
    identity += device.to_string();
    return identity;
}

constexpr Grant kCreateRooms{.permission = Permission::CreateRooms, .room = {}, .identity = {}};
constexpr Grant kRecordRooms{.permission = Permission::RecordRoom, .room = {}, .identity = {}};

class LiveKitRoom final : public IMediaRoom {
public:
    LiveKitRoom(RoomService& service, const Endpoints& endpoints, std::string name,
                std::uint16_t max_participants) noexcept
        : service_(service), endpoints_(endpoints), name_(std::move(name)),
          max_participants_(max_participants) {}

    void join(const core::UserId& user, const core::DeviceId& device, MediaRole role,
              core::ports::TicketDone done) override {
        if (closed_) {
            service_.fail(
                MediaError::Closed,
                [done = std::move(done)](std::expected<void, MediaError> r) mutable noexcept {
                    done(std::unexpected(r.error()));
                });
            return;
        }
        std::string identity = identity_of(user, device);
        const bool member = role == MediaRole::Member;
        // Copies, not this: the room handle may be gone by the time the room is back.
        service_.call("RoomService/CreateRoom", create_room_body(name_, max_participants_),
                      kCreateRooms, IfAbsent::Fail,
                      [&service = service_, endpoint = member ? endpoints_.client : endpoints_.whip,
                       permission = member ? Permission::JoinRoom : Permission::PublishToRoom,
                       ttl = member ? kTicketTtl : kPublisherTicketTtl, room = name_,
                       identity = std::move(identity), done = std::move(done)](
                          std::expected<void, MediaError> opened) mutable noexcept {
                          if (!opened) {
                              done(std::unexpected(opened.error()));
                              return;
                          }
                          auto token = detail::mint_token(
                              service.key(),
                              Grant{.permission = permission, .room = room, .identity = identity},
                              service.clock().wall_now(), ttl);
                          if (!token) {
                              done(std::unexpected(MediaError::Refused));
                              return;
                          }
                          done(MediaTicket{.endpoint = std::move(endpoint),
                                           .credential = std::move(token->jwt),
                                           .expires_at = token->expires_at});
                      });
    }

    void relay(const core::UserId& user, const core::DeviceId& device,
               const core::ports::MediaRelay& target, MediaDone done) override {
        if (closed_) {
            service_.fail(MediaError::Closed, std::move(done));
            return;
        }
        // The packager listens only with SRT (ADR-0046); anything else is a caller's mistake.
        if (!target.url.starts_with("srt://")) {
            service_.fail(MediaError::Refused, std::move(done));
            return;
        }
        service_.call("Egress/StartParticipantEgress",
                      relay_body(name_, identity_of(user, device), target), kRecordRooms,
                      IfAbsent::Fail, std::move(done));
    }

    void close(MediaDone done) override {
        closed_ = true;
        std::string body = R"({"room":)";
        core::json::append_string(body, name_);
        body += '}';
        service_.call("RoomService/DeleteRoom", std::move(body), kCreateRooms, IfAbsent::Succeed,
                      std::move(done));
    }

private:
    RoomService& service_;
    const Endpoints& endpoints_;
    std::string name_;
    std::uint16_t max_participants_;
    bool closed_ = false;
};

class LiveKitSfu final : public core::ports::ISfu {
public:
    LiveKitSfu(net::IReactor& reactor, curl::Multi& multi, const core::ports::IClock& clock,
               Config config)
        : endpoints_{.client = config.client_url, .whip = whip_url(config.client_url)},
          service_(reactor, multi, clock, std::move(config.api_url),
                   detail::ApiKey{.id = std::move(config.api_key),
                                  .secret = std::move(config.api_secret)}) {}

    void open_room(const core::RoomId& room, core::ports::MediaGeneration generation,
                   std::uint16_t max_participants, OpenDone done) override {
        // A room id never holds ':', so every generation of every room has a name of its own.
        std::string name = room.to_string();
        name += ':';
        name += std::to_string(std::to_underlying(generation));
        std::string body = create_room_body(name, max_participants);
        service_.call("RoomService/CreateRoom", std::move(body), kCreateRooms, IfAbsent::Fail,
                      [this, name = std::move(name), max_participants, done = std::move(done)](
                          std::expected<void, MediaError> created) mutable noexcept {
                          if (!created) {
                              done(std::unexpected(created.error()));
                              return;
                          }
                          done(std::make_unique<LiveKitRoom>(service_, endpoints_, std::move(name),
                                                             max_participants));
                      });
    }

private:
    Endpoints endpoints_;
    RoomService service_;
};

[[nodiscard]] bool has_scheme(std::string_view url, std::string_view plain,
                              std::string_view secure) noexcept {
    const auto opens_with = [url](std::string_view scheme) {
        return url.starts_with(scheme) && url.size() > scheme.size();
    };
    return opens_with(plain) || opens_with(secure);
}

} // namespace

std::string_view to_string(ConfigError e) noexcept {
    switch (e) {
    case ConfigError::BadApiUrl:
        return "livekit api url must be http:// or https://";
    case ConfigError::BadClientUrl:
        return "livekit client url must be ws:// or wss://";
    case ConfigError::MissingApiKey:
        return "livekit api key missing";
    case ConfigError::BadApiSecret:
        return "livekit api secret must be 32 to 256 bytes";
    }
    return "unknown livekit config error";
}

std::expected<std::unique_ptr<core::ports::ISfu>, ConfigError>
make_sfu(net::IReactor& reactor, curl::Multi& multi, const core::ports::IClock& clock,
         Config config) {
    while (config.api_url.ends_with('/')) {
        config.api_url.pop_back();
    }
    if (!has_scheme(config.api_url, "http://", "https://")) {
        return std::unexpected(ConfigError::BadApiUrl);
    }
    if (!has_scheme(config.client_url, "ws://", "wss://")) {
        return std::unexpected(ConfigError::BadClientUrl);
    }
    if (config.api_key.empty()) {
        return std::unexpected(ConfigError::MissingApiKey);
    }
    if (config.api_secret.size() < kMinSecretBytes || config.api_secret.size() > kMaxSecretBytes) {
        return std::unexpected(ConfigError::BadApiSecret);
    }
    return std::make_unique<LiveKitSfu>(reactor, multi, clock, std::move(config));
}

} // namespace infra::sfu::livekit
