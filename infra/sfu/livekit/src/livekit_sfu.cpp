#include "infra/sfu/livekit/livekit_sfu.hpp"

#include "core/util/json.hpp"
#include "core/util/parse.hpp"

#include "access_token.hpp"
#include "room_service.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace infra::sfu::livekit {

namespace {

using core::ports::IMediaRoom;
using core::ports::MediaDone;
using core::ports::MediaError;
using core::ports::MediaRole;
using core::ports::MediaRoomKind;
using core::ports::MediaTicket;
using detail::Answer;
using detail::CallLimits;
using detail::Grant;
using detail::IfAbsent;
using detail::Permission;
using detail::RoomService;

// A ticket is presented once, to connect: from then on LiveKit keeps the client's token fresh
// itself (at join and every 5 min, pkg/service/roommanager.go in v1.13.7), so reconnects never
// need the ticket again. One connect is the SDK's 15 s signal plus 15 s peer-connection budget,
// and room.connect() retries once: 60 s. Clock skew needs nothing extra, LiveKit allows a minute.
// A publisher's ticket lives no longer: a WHIP POST with it re-creates its room even after the
// generation was closed (LiveKit v1.13.7 skips the auto_create check on that path), so each
// PATCH and the DELETE go out with a fresh ticket instead of a long one (ADR-0053).
constexpr core::Seconds kTicketTtl{2 * (15 + 15)};
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
// serves on the same port over HTTP; and where the relay calls packagers.
struct Endpoints {
    std::string client;
    std::string whip;
    std::string packager;
};

// RFC 6455 section 3: ws and wss URIs are http and https ones under other names, so the WHIP
// endpoint is the client URL, checked to be ws or wss by make_sfu, with "ws" read as "http".
std::string whip_url(std::string_view client_url) {
    std::string url = "http";
    url += client_url.substr(std::string_view("ws").size());
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
// The packager's segment lengths (ADR-0046).
constexpr core::Seconds kMinKeyframeInterval{2};
constexpr core::Seconds kMaxKeyframeInterval{10};
// The packager's stream ids and SRT's passphrase bounds (live_packager's StreamId, ADR-0046).
constexpr std::size_t kMaxStreamId = 64;
constexpr std::size_t kMinPassphrase = 10;
constexpr std::size_t kMaxPassphrase = 79;

// Starting a relay took 530 to 541 ms over seven local runs: 500 ms of it is LiveKit's RPC
// waiting for a busier recorder to bid before it takes an idle one's (ShortCircuitTimeout in
// protocol's rpc/egress_client.go), the rest the recorder launching its handler. Ten times that
// still reports a stuck start while the stream service can act on it.
constexpr CallLimits kStartRelayLimits{.timeout = core::Millis{5000},
                                       .max_response = std::size_t{16} * 1024};
// Each EgressInfo is about 3 KiB; a room's active relays are one, a few during a hand-over.
constexpr CallLimits kListRelayLimits{.timeout = core::Millis{5000},
                                      .max_response = std::size_t{64} * 1024};
// One ParticipantInfo, its tracks and their codecs: a few KiB.
constexpr CallLimits kPresenceLimits{.timeout = core::Millis{5000},
                                     .max_response = std::size_t{64} * 1024};

[[nodiscard]] bool valid_stream_id(std::string_view id) noexcept {
    return !id.empty() && id.size() <= kMaxStreamId && std::ranges::all_of(id, [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == '_' || c == '-';
    });
}

// A stream id put into a host name must be a DNS label (RFC 1123): lowercase letters, digits
// and '-', not at either end, 63 characters at most. Refused rather than rewritten, so two ids
// never name one host.
constexpr std::size_t kMaxDnsLabel = 63;
[[nodiscard]] bool valid_host_label(std::string_view id) noexcept {
    return !id.empty() && id.size() <= kMaxDnsLabel && id.front() != '-' && id.back() != '-' &&
           std::ranges::all_of(id, [](char c) {
               return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
           });
}

constexpr std::string_view kStreamPlaceholder = "{stream}";

[[nodiscard]] bool valid_passphrase(std::string_view passphrase) noexcept {
    return passphrase.size() >= kMinPassphrase && passphrase.size() <= kMaxPassphrase &&
           std::ranges::all_of(passphrase, [](char c) { return c >= ' ' && c <= '~'; });
}

void append_percent_encoded(std::string& out, std::string_view text) {
    constexpr std::string_view kHex = "0123456789ABCDEF";
    for (const char c : text) {
        const bool unreserved = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                                (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
                                c == '~';
        if (unreserved) {
            out += c;
            continue;
        }
        const auto byte = static_cast<unsigned char>(c);
        out += '%';
        out += kHex[byte >> 4U];
        out += kHex[byte & 0x0FU];
    }
}

// The packager's listener for `target`: the configured address, "{stream}" in it replaced by
// the stream's id, with the stream id and passphrase its handshake checks (ADR-0046).
std::string packager_url(std::string_view configured, const core::ports::MediaRelay& target) {
    std::string url;
    constexpr std::string_view kPlaceholder = kStreamPlaceholder;
    const auto at = configured.find(kPlaceholder);
    if (at == std::string_view::npos) {
        url = configured;
    } else {
        url = configured.substr(0, at);
        url += target.stream;
        url += configured.substr(at + kPlaceholder.size());
    }
    url += "?streamid=";
    url += target.stream;
    url += "&passphrase=";
    append_percent_encoded(url, target.passphrase);
    return url;
}

std::string relay_body(std::string_view room, std::string_view identity, std::string_view url,
                       core::Seconds keyframe_interval) {
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
    body += std::to_string(keyframe_interval.count());
    body += R"(},"stream_outputs":[{"protocol":"SRT","urls":[)";
    core::json::append_string(body, url);
    body += "]}]}";
    return body;
}

// The id of a relay LiveKit runs for `identity`, from a ListEgress answer. `active` there also
// lists egresses that are ending, which will not carry the stream, so only starting and active
// ones count. nullopt when there is none; an unreadable answer is an error.
std::expected<std::optional<std::string>, MediaError> running_relay(std::string_view answer,
                                                                    std::string_view identity) {
    const auto parsed = core::json::parse(answer);
    if (!parsed) {
        return std::unexpected(MediaError::Unavailable);
    }
    const core::json::Value* items = parsed->find("items");
    if (items == nullptr || items->as_array() == nullptr) {
        return std::nullopt;
    }
    for (const core::json::Value& item : *items->as_array()) {
        const core::json::Value* participant = item.find("participant");
        const core::json::Value* named =
            participant != nullptr ? participant->find("identity") : nullptr;
        const core::json::Value* status = item.find("status");
        const bool live = status != nullptr && (status->as_string() == "EGRESS_STARTING" ||
                                                status->as_string() == "EGRESS_ACTIVE");
        if (!live || named == nullptr || named->as_string() != identity) {
            continue;
        }
        const core::json::Value* id = item.find("egress_id");
        const auto text = id != nullptr ? id->as_string() : std::nullopt;
        if (text.has_value()) {
            return std::string(*text);
        }
    }
    return std::nullopt;
}

// A room's participants, each with its tracks, about 1 to 2 KiB apiece; a group call holds 16
// at most (ADR-0095), and 64 KiB leaves room for LiveKit's own recorder joining one.
constexpr CallLimits kListParticipantsLimits{.timeout = core::Millis{5000},
                                             .max_response = std::size_t{64} * 1024};

// A JSON integer LiveKit's Twirp may write either way: protojson writes int64 as a string.
std::optional<std::int64_t> integer_of(const core::json::Value* value) noexcept {
    if (value == nullptr) {
        return std::nullopt;
    }
    if (const auto number = value->as_i64()) {
        return number;
    }
    const auto text = value->as_string();
    if (!text) {
        return std::nullopt;
    }
    return core::parse_integer<std::int64_t>(*text);
}

// The members connected to a room, from a ListParticipants answer: identities that are not
// "<user>/<device>" (LiveKit's recorder, a WHIP source) are not members, and participants that
// are leaving no longer count. A room LiveKit does not have holds nobody: IfAbsent::Succeed hands
// over its Twirp not_found error, `{"code":"not_found","msg":...}`, as the answer, as it does for
// present(). protojson leaves an empty list out.
std::expected<std::vector<core::ports::MediaParticipant>, MediaError>
participants_of(std::string_view answer) {
    std::vector<core::ports::MediaParticipant> out;
    const auto parsed = core::json::parse(answer);
    if (!parsed) {
        return std::unexpected(MediaError::Unavailable);
    }
    if (const core::json::Value* code = parsed->find("code");
        code != nullptr && code->as_string() == "not_found") {
        return out;
    }
    const core::json::Value* list = parsed->find("participants");
    if (list == nullptr || list->as_array() == nullptr) {
        return out;
    }
    for (const core::json::Value& p : *list->as_array()) {
        const core::json::Value* identity = p.find("identity");
        const auto text = identity != nullptr ? identity->as_string() : std::nullopt;
        const core::json::Value* state = p.find("state");
        if (!text || (state != nullptr && state->as_string() == "DISCONNECTED")) {
            continue;
        }
        const auto slash = text->find('/');
        if (slash == std::string_view::npos) {
            continue;
        }
        const auto user = core::UserId::parse(text->substr(0, slash));
        const auto device = core::DeviceId::parse(text->substr(slash + 1));
        if (!user || !device) {
            continue;
        }
        const auto ms = integer_of(p.find("joined_at_ms"));
        const auto seconds = integer_of(p.find("joined_at"));
        const core::Millis since =
            ms ? core::Millis{*ms} : core::Millis{seconds.value_or(0) * 1000};
        out.push_back({.user = *user, .device = *device, .joined_at = core::WallTime{since}});
    }
    return out;
}

std::string identity_of(const core::UserId& user, const core::DeviceId& device) {
    // A user id never holds '/', so the identity splits back apart unambiguously.
    std::string identity(user.view());
    identity += '/';
    identity += device.to_string();
    return identity;
}

// A room id never holds ':', so every generation of every room has a name of its own.
std::string room_name(const core::RoomId& room, core::ports::MediaGeneration generation) {
    std::string name = room.to_string();
    name += ':';
    name += std::to_string(std::to_underlying(generation));
    return name;
}

// A GetParticipant answer: the participant, unless LiveKit already counts it gone; or, passed
// through as success, LiveKit's not_found for the participant or for its room.
[[nodiscard]] std::expected<bool, MediaError> connected(std::string_view answer) {
    const auto info = core::json::parse(answer);
    if (!info) {
        return std::unexpected(MediaError::Unavailable);
    }
    const core::json::Value* code = info->find("code");
    if (code != nullptr && code->as_string() == "not_found") {
        return false;
    }
    if (info->find("identity") == nullptr) {
        return std::unexpected(MediaError::Unavailable);
    }
    const core::json::Value* state = info->find("state");
    return state == nullptr || state->as_string() != "DISCONNECTED";
}

// A ListRooms answer for one name: whether the room is there.
[[nodiscard]] std::expected<bool, MediaError> room_listed(std::string_view answer) {
    const auto info = core::json::parse(answer);
    const core::json::Value* rooms = info ? info->find("rooms") : nullptr;
    // protojson leaves an empty list out.
    if (info && rooms == nullptr && info->as_object() != nullptr) {
        return false;
    }
    if (rooms == nullptr || rooms->as_array() == nullptr) {
        return std::unexpected(MediaError::Unavailable);
    }
    return !rooms->as_array()->empty();
}

constexpr Grant kListRooms{.permission = Permission::ListRooms, .room = {}, .identity = {}};
constexpr Grant kCreateRooms{.permission = Permission::CreateRooms, .room = {}, .identity = {}};
constexpr Grant kRecordRooms{.permission = Permission::RecordRoom, .room = {}, .identity = {}};

using RelayResult = std::expected<std::string, MediaError>;

// Relay starts in flight, by room and identity. LiveKit records an egress only once its start
// has answered, about half a second later, so a second relay() inside that window would list
// nothing and start another: it waits for the first one's answer instead. Reactor thread only.
class RelayStarts {
public:
    explicit RelayStarts(RoomService& service) noexcept : service_(service) {}

    // `start` is the StartParticipantEgress body, used if LiveKit runs none for `identity` yet.
    void run(std::string_view room, std::string_view identity, std::string start,
             core::ports::RelayDone done) {
        std::string key(room);
        key += '\n';
        key += identity;
        const auto [waiters, first] = waiting_.try_emplace(key);
        waiters->second.push_back(std::move(done));
        if (!first) {
            return;
        }
        std::string list = R"({"room_name":)";
        core::json::append_string(list, room);
        list += R"(,"active":true})";
        service_.fetch("Egress/ListEgress", std::move(list), kRecordRooms, kListRelayLimits,
                       [this, key = std::move(key), identity = std::string(identity),
                        start = std::move(start)](Answer listed) mutable noexcept {
                           if (!listed) {
                               finish(key, std::unexpected(listed.error()));
                               return;
                           }
                           auto running = running_relay(*listed, identity);
                           if (!running) {
                               finish(key, std::unexpected(running.error()));
                               return;
                           }
                           if (*running) {
                               finish(key, std::move(**running));
                               return;
                           }
                           service_.fetch("Egress/StartParticipantEgress", std::move(start),
                                          kRecordRooms, kStartRelayLimits,
                                          [this, key = std::move(key)](Answer started) noexcept {
                                              finish(key, started_relay(started));
                                          });
                       });
    }

private:
    static RelayResult started_relay(const Answer& started) {
        if (!started) {
            return std::unexpected(started.error());
        }
        const auto info = core::json::parse(*started);
        const core::json::Value* id = info ? info->find("egress_id") : nullptr;
        const auto text = id != nullptr ? id->as_string() : std::nullopt;
        // Started, perhaps, with no id to show for it: the retry's listing will find it.
        if (!text.has_value()) {
            return std::unexpected(MediaError::Unavailable);
        }
        return std::string(*text);
    }

    void finish(const std::string& key, const RelayResult& result) noexcept {
        const auto found = waiting_.find(key);
        if (found == waiting_.end()) {
            return;
        }
        // Taken out first: a callback may destroy the ISfu, and this with it.
        std::vector<core::ports::RelayDone> waiters = std::move(found->second);
        waiting_.erase(found);
        for (core::ports::RelayDone& done : waiters) {
            done(result);
        }
    }

    RoomService& service_;
    std::map<std::string, std::vector<core::ports::RelayDone>, std::less<>> waiting_;
};

class LiveKitRoom final : public IMediaRoom {
public:
    LiveKitRoom(RoomService& service, RelayStarts& relays, const Endpoints& endpoints,
                std::string name, MediaRoomKind kind, std::uint16_t max_participants) noexcept
        : service_(service), relays_(relays), endpoints_(endpoints), name_(std::move(name)),
          kind_(kind), max_participants_(max_participants) {}

    void join(const core::UserId& user, const core::DeviceId& device, MediaRole role,
              core::ports::TicketDone done) override {
        const bool member = role == MediaRole::Member;
        // A call admits members and a stream its publisher, nothing else: a publisher ticket
        // for a call's generation could bring it back after it was closed to put someone out
        // (ADR-0053), and members never join a stream's room (ADR-0014).
        const bool fits = member == (kind_ == MediaRoomKind::Call);
        if (closed_ || !fits) {
            service_.fail(closed_ ? MediaError::Closed : MediaError::Refused,
                          [done = std::move(done)](Answer r) mutable noexcept {
                              done(std::unexpected(r.error()));
                          });
            return;
        }
        std::string identity = identity_of(user, device);
        // Copies, not this: the room handle may be gone by the time the room is back.
        service_.call("RoomService/CreateRoom", create_room_body(name_, max_participants_),
                      kCreateRooms, IfAbsent::Fail,
                      [&service = service_, endpoint = member ? endpoints_.client : endpoints_.whip,
                       permission = member ? Permission::JoinRoom : Permission::PublishToRoom,
                       room = name_, identity = std::move(identity), done = std::move(done)](
                          std::expected<void, MediaError> opened) mutable noexcept {
                          if (!opened) {
                              done(std::unexpected(opened.error()));
                              return;
                          }
                          auto token = detail::mint_token(
                              service.key(),
                              Grant{.permission = permission, .room = room, .identity = identity},
                              service.clock().wall_now(), kTicketTtl);
                          if (!token) {
                              done(std::unexpected(MediaError::Refused));
                              return;
                          }
                          done(MediaTicket{.endpoint = std::move(endpoint),
                                           .credential = std::move(token->jwt),
                                           .expires_at = token->expires_at});
                      });
    }

    void participants(core::ports::ParticipantsDone done) override {
        if (closed_) {
            service_.fail(MediaError::Closed, [done = std::move(done)](Answer r) mutable noexcept {
                done(std::unexpected(r.error()));
            });
            return;
        }
        std::string body = R"({"room":)";
        core::json::append_string(body, name_);
        body += '}';
        // A room LiveKit dropped for standing empty holds nobody: not found is an answer.
        service_.fetch(
            "RoomService/ListParticipants", std::move(body),
            Grant{.permission = Permission::AdminRoom, .room = name_, .identity = {}},
            kListParticipantsLimits,
            [done = std::move(done)](Answer listed) mutable noexcept {
                if (!listed) {
                    done(std::unexpected(listed.error()));
                    return;
                }
                try {
                    done(participants_of(*listed));
                } catch (const std::bad_alloc&) {
                    done(std::unexpected(MediaError::Unavailable));
                }
            },
            IfAbsent::Succeed);
    }

    void relay(const core::UserId& user, const core::DeviceId& device,
               const core::ports::MediaRelay& target, core::ports::RelayDone done) override {
        const auto refuse = [&](MediaError error) {
            service_.fail(error, [done = std::move(done)](Answer r) mutable noexcept {
                done(std::unexpected(r.error()));
            });
        };
        if (closed_) {
            refuse(MediaError::Closed);
            return;
        }
        if (kind_ != MediaRoomKind::Stream || endpoints_.packager.empty() ||
            !valid_stream_id(target.stream) || !valid_passphrase(target.passphrase) ||
            (endpoints_.packager.contains(kStreamPlaceholder) &&
             !valid_host_label(target.stream)) ||
            target.keyframe_interval < kMinKeyframeInterval ||
            target.keyframe_interval > kMaxKeyframeInterval) {
            refuse(MediaError::Refused);
            return;
        }
        // Listed first, so that a retry after a lost answer finds the relay the first attempt
        // started instead of starting another.
        const std::string identity = identity_of(user, device);
        relays_.run(name_, identity,
                    relay_body(name_, identity, packager_url(endpoints_.packager, target),
                               target.keyframe_interval),
                    std::move(done));
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
    RelayStarts& relays_;
    const Endpoints& endpoints_;
    std::string name_;
    MediaRoomKind kind_;
    std::uint16_t max_participants_;
    bool closed_ = false;
};

class LiveKitSfu final : public core::ports::ISfu {
public:
    LiveKitSfu(net::IReactor& reactor, curl::Multi& multi, const core::ports::IClock& clock,
               Config config)
        : endpoints_{.client = config.client_url,
                     .whip = whip_url(config.client_url),
                     .packager = std::move(config.packager_srt)},
          service_(reactor, multi, clock, std::move(config.api_url),
                   detail::ApiKey{.id = std::move(config.api_key),
                                  .secret = std::move(config.api_secret)}) {}

    void open_room(const core::RoomId& room, core::ports::MediaGeneration generation,
                   MediaRoomKind kind, std::uint16_t max_participants, OpenDone done) override {
        std::string name = room_name(room, generation);
        std::string body = create_room_body(name, max_participants);
        service_.call(
            "RoomService/CreateRoom", std::move(body), kCreateRooms, IfAbsent::Fail,
            [this, name = std::move(name), kind, max_participants,
             done = std::move(done)](std::expected<void, MediaError> created) mutable noexcept {
                if (!created) {
                    done(std::unexpected(created.error()));
                    return;
                }
                done(std::make_unique<LiveKitRoom>(service_, relays_, endpoints_, std::move(name),
                                                   kind, max_participants));
            });
    }

    void present(const core::RoomId& room, core::ports::MediaGeneration generation,
                 const core::UserId& user, const core::DeviceId& device,
                 core::ports::PresenceDone done) override {
        std::string name = room_name(room, generation);
        std::string list = R"({"names":[)";
        core::json::append_string(list, name);
        list += "]}";
        // The room first, by name: GetParticipant in a room no node holds answers unavailable
        // (LiveKit v1.13.7 routes it to the room's node), not not_found. Neither call opens the
        // room, so one LiveKit has dropped stays dropped.
        service_.fetch(
            "RoomService/ListRooms", std::move(list), kListRooms, kPresenceLimits,
            [this, name = std::move(name), identity = identity_of(user, device),
             done = std::move(done)](Answer listed) mutable noexcept {
                if (!listed) {
                    done(std::unexpected(listed.error()));
                    return;
                }
                const auto exists = room_listed(*listed);
                if (!exists || !*exists) {
                    done(exists ? std::expected<bool, MediaError>(false)
                                : std::unexpected(exists.error()));
                    return;
                }
                std::string body = R"({"room":)";
                core::json::append_string(body, name);
                body += R"(,"identity":)";
                core::json::append_string(body, identity);
                body += '}';
                // IfAbsent::Succeed hands LiveKit's not_found over as an answer, for connected()
                // to read.
                service_.fetch(
                    "RoomService/GetParticipant", std::move(body),
                    Grant{.permission = Permission::AdminRoom, .room = name, .identity = {}},
                    kPresenceLimits,
                    [done = std::move(done)](Answer answer) mutable noexcept {
                        if (!answer) {
                            done(std::unexpected(answer.error()));
                            return;
                        }
                        done(connected(*answer));
                    },
                    IfAbsent::Succeed);
            });
    }

private:
    Endpoints endpoints_;
    RoomService service_;
    RelayStarts relays_{service_};
};

[[nodiscard]] bool has_scheme(std::string_view url, std::string_view plain,
                              std::string_view secure) noexcept {
    const auto opens_with = [url](std::string_view scheme) {
        return url.starts_with(scheme) && url.size() > scheme.size();
    };
    return opens_with(plain) || opens_with(secure);
}

// "srt://<host>:<port>", where the host may hold "{stream}"; the query is the relay's to add.
[[nodiscard]] bool valid_packager_address(std::string_view address) noexcept {
    constexpr std::string_view kScheme = "srt://";
    if (!address.starts_with(kScheme) ||
        address.find_first_of("?#/", kScheme.size()) != std::string_view::npos) {
        return false;
    }
    const auto colon = address.rfind(':');
    if (colon == std::string_view::npos || colon <= kScheme.size()) {
        return false;
    }
    const std::string_view port = address.substr(colon + 1);
    return core::parse_integer<std::uint16_t>(port).has_value();
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
    case ConfigError::BadPackagerAddress:
        return "packager address must be srt://<host>:<port> with no query";
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
    if (!config.packager_srt.empty() && !valid_packager_address(config.packager_srt)) {
        return std::unexpected(ConfigError::BadPackagerAddress);
    }
    return std::make_unique<LiveKitSfu>(reactor, multi, clock, std::move(config));
}

} // namespace infra::sfu::livekit
