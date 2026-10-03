#pragma once

#include "core/models/ids.hpp"
#include "core/util/time.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

namespace gateway {

// What LiveKit signs its webhooks with (ADR-0093): the API key pair named by its configuration's
// webhook.api_key, the same pair the stream service calls LiveKit with.
struct WebhookKey {
    std::string id;
    std::string secret;
};

// Why a webhook was refused. Every one of them is answered 401 but TooLarge (413): nothing about
// the request is believed until its token's signature, issuer, lifetime and body hash all hold.
enum class WebhookRejection : std::uint8_t {
    // No Authorization header, or an empty one.
    NoAuthorization,
    // Not a JWS of three base64url parts with JSON in the first two, or longer than any token
    // LiveKit makes.
    Malformed,
    // A token not signed with HS256, the only algorithm LiveKit signs webhooks with.
    Algorithm,
    // The HMAC does not match: another secret, or a token or body changed on the way.
    Signature,
    // Signed with our secret but issued for another key.
    UnknownKey,
    // No `exp`: such a token would never expire.
    NoExpiry,
    Expired,
    NotYetValid,
    // No `sha256` claim, or not the body's.
    BodyHash,
    // Past kMaxWebhookBody.
    TooLarge,
};

inline constexpr std::size_t kWebhookRejections = 10;

[[nodiscard]] std::string_view to_string(WebhookRejection r) noexcept;

// LiveKit's largest event is a participant with its tracks and their codecs, a few KiB; a room
// with long metadata stays far under this.
inline constexpr std::size_t kMaxWebhookBody = std::size_t{64} * 1024;
// A LiveKit token is a few hundred bytes.
inline constexpr std::size_t kMaxWebhookToken = std::size_t{4} * 1024;
// The clock skew LiveKit's own receiver allows (auth/verifier.go, protocol of v1.13.7).
inline constexpr core::Seconds kWebhookLeeway{60};

// Checks a webhook as LiveKit's own receiver does (webhook.Receive, protocol of LiveKit
// v1.13.7): the Authorization header is a bare HS256 JWT issued by `key.id`, signed with
// `key.secret`, unexpired at `now` within the leeway, whose `sha256` claim is the standard
// base64 of the body's SHA-256. The signature and the body hash are compared in constant time.
[[nodiscard]] std::expected<void, WebhookRejection> verify_webhook(std::string_view authorization,
                                                                   std::string_view body,
                                                                   const WebhookKey& key,
                                                                   core::WallTime now);

// The events the stream service acts on; every other is Other and only acknowledged.
enum class WebhookEventKind : std::uint8_t {
    ParticipantJoined,
    TrackPublished,
    ParticipantLeft,
    // LiveKit gave up on a participant that never finished connecting.
    ParticipantAborted,
    RoomFinished,
    Other,
};

// A verified webhook's body, as protojson writes a livekit.WebhookEvent: lowerCamelCase names,
// int64 as strings, unset fields left out.
// NOLINTBEGIN(readability-redundant-member-init): every member has an initializer, so a
// designated initializer may name only those it sets.
struct WebhookEvent {
    WebhookEventKind kind = WebhookEventKind::Other;
    // The event's id at LiveKit, for the log.
    std::string id = {};
    std::string room = {};
    std::string identity = {};
    // The participant's session: a client that reconnects in full comes back with a new one.
    std::string participant_sid = {};
};
// NOLINTEND(readability-redundant-member-init)

[[nodiscard]] std::string_view to_string(WebhookEventKind k) noexcept;

// nullopt for a body that is not a JSON object with a string `event`.
[[nodiscard]] std::optional<WebhookEvent> parse_webhook_event(std::string_view body);

// The stream a LiveKit room belongs to: the stream service names a stream's room
// "<stream id>:1", its one generation (ADR-0091). Anything else is no stream's room.
[[nodiscard]] std::optional<core::LiveStreamId> stream_of_room(std::string_view room);

// The owner, when `identity` is the publisher of `stream`: "<owner>/<stream id>", the identity
// every one of the stream's tickets names. A call's members, LiveKit's recorder and anyone else
// are not.
[[nodiscard]] std::optional<core::UserId> publisher_of(std::string_view identity,
                                                       const core::LiveStreamId& stream);

} // namespace gateway
