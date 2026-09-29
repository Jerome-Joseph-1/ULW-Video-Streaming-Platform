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

using core::ports::IMediaParticipant;
using core::ports::IMediaRoom;
using core::ports::MediaDone;
using core::ports::MediaError;
using core::ports::MediaTicket;
using detail::Grant;
using detail::IfAbsent;
using detail::Permission;
using detail::RoomService;

// A ticket is used once, to connect, but LiveKit also checks it when the client resumes a
// dropped connection, until it pushes the participant a fresh token: every 5 minutes
// (tokenRefreshInterval, pkg/service/roommanager.go in v1.13.7). Covering that first refresh
// and the SDK's 30 s connect budget (15 s signal plus 15 s peer connection) gives 5.5 min,
// rounded up to 6. A participant removed from the room can rejoin with its ticket for that long;
// the signalling path withholds new tickets, not the old one.
constexpr core::Seconds kTicketTtl{6 * 60};
// A room nobody has joined yet is kept while the tickets issued when it opened are still good.
constexpr core::Seconds kEmptyRoomTimeout = kTicketTtl;

// RFC 7518 section 3.2: an HS256 key of at least 256 bits.
constexpr std::size_t kMinSecretBytes = 256 / 8;
// Generated secrets are 43 to 64 characters; 256 bytes is far past any real one.
constexpr std::size_t kMaxSecretBytes = 256;

class LiveKitParticipant final : public IMediaParticipant {
public:
    LiveKitParticipant(RoomService& service, std::string room, std::string identity,
                       MediaTicket ticket) noexcept
        : service_(service), room_(std::move(room)), identity_(std::move(identity)),
          ticket_(std::move(ticket)) {}

    [[nodiscard]] const MediaTicket& ticket() const noexcept override { return ticket_; }

    void remove(MediaDone done) override {
        std::string body = R"({"room":)";
        core::json::append_string(body, room_);
        body += R"(,"identity":)";
        core::json::append_string(body, identity_);
        body += '}';
        service_.call(
            "RemoveParticipant", std::move(body),
            Grant{.permission = Permission::AdministerRoom, .room = room_, .identity = {}},
            IfAbsent::Succeed, std::move(done));
    }

private:
    RoomService& service_;
    std::string room_;
    std::string identity_;
    MediaTicket ticket_;
};

class LiveKitRoom final : public IMediaRoom {
public:
    LiveKitRoom(RoomService& service, std::string client_url, std::string name) noexcept
        : service_(service), client_url_(std::move(client_url)), name_(std::move(name)) {}

    [[nodiscard]] std::expected<std::unique_ptr<IMediaParticipant>, MediaError>
    join(const core::UserId& user, const core::DeviceId& device) override {
        // A user id never holds '/', so the identity splits back apart unambiguously.
        std::string identity(user.view());
        identity += '/';
        identity += device.to_string();
        auto token = detail::mint_token(
            service_.key(),
            Grant{.permission = Permission::JoinRoom, .room = name_, .identity = identity},
            service_.clock().wall_now(), kTicketTtl);
        if (!token) {
            return std::unexpected(MediaError::Refused);
        }
        return std::make_unique<LiveKitParticipant>(service_, name_, std::move(identity),
                                                    MediaTicket{.endpoint = client_url_,
                                                                .credential = std::move(token->jwt),
                                                                .expires_at = token->expires_at});
    }

    void close(MediaDone done) override {
        std::string body = R"({"room":)";
        core::json::append_string(body, name_);
        body += '}';
        service_.call("DeleteRoom", std::move(body),
                      Grant{.permission = Permission::CreateRooms, .room = {}, .identity = {}},
                      IfAbsent::Succeed, std::move(done));
    }

private:
    RoomService& service_;
    std::string client_url_;
    std::string name_;
};

class LiveKitSfu final : public core::ports::ISfu {
public:
    LiveKitSfu(net::IReactor& reactor, curl::Multi& multi, const core::ports::IClock& clock,
               Config config)
        : client_url_(std::move(config.client_url)),
          service_(reactor, multi, clock, std::move(config.api_url),
                   detail::ApiKey{.id = std::move(config.api_key),
                                  .secret = std::move(config.api_secret)}) {}

    void open_room(const core::RoomId& room, std::uint16_t max_participants,
                   OpenDone done) override {
        std::string name = room.to_string();
        std::string body = R"({"name":)";
        core::json::append_string(body, name);
        body += R"(,"empty_timeout":)";
        body += std::to_string(kEmptyRoomTimeout.count());
        body += R"(,"max_participants":)";
        body += std::to_string(max_participants);
        body += '}';
        service_.call(
            "CreateRoom", std::move(body),
            Grant{.permission = Permission::CreateRooms, .room = {}, .identity = {}},
            IfAbsent::Fail,
            [this, name = std::move(name),
             done = std::move(done)](std::expected<void, MediaError> created) mutable noexcept {
                if (!created) {
                    done(std::unexpected(created.error()));
                    return;
                }
                done(std::make_unique<LiveKitRoom>(service_, client_url_, std::move(name)));
            });
    }

private:
    std::string client_url_;
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
