#include "livekit_webhook.hpp"

#include "core/util/json.hpp"
#include "infra/auth/base64url.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <climits>
#include <cstddef>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>

namespace gateway {

namespace {

constexpr std::size_t kMacBytes = 32;
// Standard base64 of 32 bytes, with its one '=' of padding.
constexpr std::size_t kHashText = 44;

// OpenSSL takes bytes as unsigned char.
const unsigned char* bytes_of(std::string_view s) noexcept {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    return reinterpret_cast<const unsigned char*>(s.data());
}

std::optional<std::array<unsigned char, kMacBytes>> hmac_sha256(std::string_view secret,
                                                                std::string_view input) {
    if (secret.size() > INT_MAX) {
        return std::nullopt;
    }
    std::array<unsigned char, EVP_MAX_MD_SIZE> mac{};
    unsigned int size = 0;
    if (HMAC(EVP_sha256(), secret.data(), static_cast<int>(secret.size()), bytes_of(input),
             input.size(), mac.data(), &size) == nullptr ||
        size != kMacBytes) {
        return std::nullopt;
    }
    std::array<unsigned char, kMacBytes> out{};
    std::copy_n(mac.begin(), kMacBytes, out.begin());
    return out;
}

// What LiveKit puts in the token's sha256 claim: base64.StdEncoding of the body's SHA-256.
std::array<char, kHashText + 1> body_hash(std::string_view body) {
    std::array<unsigned char, SHA256_DIGEST_LENGTH> digest{};
    SHA256(bytes_of(body), body.size(), digest.data());
    std::array<char, kHashText + 1> text{};
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): OpenSSL writes uchar.
    EVP_EncodeBlock(reinterpret_cast<unsigned char*>(text.data()), digest.data(),
                    static_cast<int>(digest.size()));
    return text;
}

std::optional<core::json::Value> json_part(std::string_view part) {
    const auto text = infra::auth::decode_base64url(part);
    if (!text) {
        return std::nullopt;
    }
    auto value = core::json::parse(*text, {.max_depth = 8, .max_bytes = kMaxWebhookToken});
    if (!value || value->as_object() == nullptr) {
        return std::nullopt;
    }
    return std::move(*value);
}

std::string string_member(const core::json::Value& object, std::string_view key) {
    const core::json::Value* v = object.find(key);
    const auto text = v != nullptr ? v->as_string() : std::nullopt;
    return text ? std::string(*text) : std::string{};
}

// NumericDate: whole seconds as LiveKit's library writes them. nullopt when absent; a value that
// is there but no integer is malformed.
std::expected<std::optional<std::int64_t>, WebhookRejection> date_member(const core::json::Value& o,
                                                                         std::string_view key) {
    const core::json::Value* v = o.find(key);
    if (v == nullptr) {
        return std::nullopt;
    }
    const auto seconds = v->as_i64();
    if (!seconds) {
        return std::unexpected(WebhookRejection::Malformed);
    }
    return *seconds;
}

std::int64_t unix_seconds(core::WallTime t) noexcept {
    return std::chrono::floor<std::chrono::seconds>(t.time_since_epoch()).count();
}

} // namespace

std::string_view to_string(WebhookRejection r) noexcept {
    switch (r) {
    case WebhookRejection::NoAuthorization:
        return "no_authorization";
    case WebhookRejection::Malformed:
        return "malformed";
    case WebhookRejection::Algorithm:
        return "algorithm";
    case WebhookRejection::Signature:
        return "signature";
    case WebhookRejection::UnknownKey:
        return "unknown_key";
    case WebhookRejection::NoExpiry:
        return "no_expiry";
    case WebhookRejection::Expired:
        return "expired";
    case WebhookRejection::NotYetValid:
        return "not_yet_valid";
    case WebhookRejection::BodyHash:
        return "body_hash";
    case WebhookRejection::TooLarge:
        return "too_large";
    }
    return "malformed";
}

std::expected<void, WebhookRejection> verify_webhook(std::string_view authorization,
                                                     std::string_view body, const WebhookKey& key,
                                                     core::WallTime now) {
    if (body.size() > kMaxWebhookBody) {
        return std::unexpected(WebhookRejection::TooLarge);
    }
    if (authorization.empty()) {
        return std::unexpected(WebhookRejection::NoAuthorization);
    }
    if (authorization.size() > kMaxWebhookToken) {
        return std::unexpected(WebhookRejection::Malformed);
    }
    const auto first = authorization.find('.');
    const auto second =
        first == std::string_view::npos ? first : authorization.find('.', first + 1);
    if (second == std::string_view::npos ||
        authorization.find('.', second + 1) != std::string_view::npos || first == 0 ||
        second == first + 1 || second + 1 == authorization.size()) {
        return std::unexpected(WebhookRejection::Malformed);
    }
    const std::string_view signed_part = authorization.substr(0, second);
    const std::string_view signature_part = authorization.substr(second + 1);

    const auto header = json_part(authorization.substr(0, first));
    if (!header) {
        return std::unexpected(WebhookRejection::Malformed);
    }
    if (string_member(*header, "alg") != "HS256") {
        return std::unexpected(WebhookRejection::Algorithm);
    }
    // The signature before any claim is believed.
    const auto expected = hmac_sha256(key.secret, signed_part);
    const auto signature = infra::auth::decode_base64url_bytes(signature_part);
    if (!expected || !signature || signature->size() != kMacBytes ||
        CRYPTO_memcmp(expected->data(), signature->data(), kMacBytes) != 0) {
        return std::unexpected(WebhookRejection::Signature);
    }
    const auto claims = json_part(authorization.substr(first + 1, second - first - 1));
    if (!claims) {
        return std::unexpected(WebhookRejection::Malformed);
    }
    if (string_member(*claims, "iss") != key.id) {
        return std::unexpected(WebhookRejection::UnknownKey);
    }
    const auto expires = date_member(*claims, "exp");
    const auto not_before = date_member(*claims, "nbf");
    if (!expires || !not_before) {
        return std::unexpected(WebhookRejection::Malformed);
    }
    if (!*expires) {
        return std::unexpected(WebhookRejection::NoExpiry);
    }
    const std::int64_t at = unix_seconds(now);
    const std::int64_t leeway = kWebhookLeeway.count();
    if (at > **expires + leeway) {
        return std::unexpected(WebhookRejection::Expired);
    }
    if (*not_before && at < **not_before - leeway) {
        return std::unexpected(WebhookRejection::NotYetValid);
    }
    const std::string claimed = string_member(*claims, "sha256");
    const auto actual = body_hash(body);
    if (claimed.size() != kHashText ||
        CRYPTO_memcmp(claimed.data(), actual.data(), kHashText) != 0) {
        return std::unexpected(WebhookRejection::BodyHash);
    }
    return {};
}

std::string_view to_string(WebhookEventKind k) noexcept {
    switch (k) {
    case WebhookEventKind::ParticipantJoined:
        return "participant_joined";
    case WebhookEventKind::TrackPublished:
        return "track_published";
    case WebhookEventKind::ParticipantLeft:
        return "participant_left";
    case WebhookEventKind::ParticipantAborted:
        return "participant_connection_aborted";
    case WebhookEventKind::RoomFinished:
        return "room_finished";
    case WebhookEventKind::Other:
        return "other";
    }
    return "other";
}

std::optional<WebhookEvent> parse_webhook_event(std::string_view body) {
    const auto parsed = core::json::parse(body, {.max_depth = 32, .max_bytes = kMaxWebhookBody});
    if (!parsed || parsed->as_object() == nullptr) {
        return std::nullopt;
    }
    const core::json::Value* name = parsed->find("event");
    const auto event = name != nullptr ? name->as_string() : std::nullopt;
    if (!event) {
        return std::nullopt;
    }
    WebhookEvent out;
    for (const WebhookEventKind k :
         {WebhookEventKind::ParticipantJoined, WebhookEventKind::TrackPublished,
          WebhookEventKind::ParticipantLeft, WebhookEventKind::ParticipantAborted,
          WebhookEventKind::RoomFinished}) {
        if (*event == to_string(k)) {
            out.kind = k;
        }
    }
    out.id = string_member(*parsed, "id");
    if (const core::json::Value* room = parsed->find("room")) {
        out.room = string_member(*room, "name");
    }
    if (const core::json::Value* participant = parsed->find("participant")) {
        out.identity = string_member(*participant, "identity");
        out.participant_sid = string_member(*participant, "sid");
    }
    return out;
}

std::optional<core::LiveStreamId> stream_of_room(std::string_view room) {
    constexpr std::string_view kGeneration = ":1";
    if (!room.ends_with(kGeneration)) {
        return std::nullopt;
    }
    const std::string_view id = room.substr(0, room.size() - kGeneration.size());
    const auto stream = core::LiveStreamId::parse(id);
    // The id's own spelling only, so one room never maps to a stream under two names.
    if (!stream || stream->to_string() != id) {
        return std::nullopt;
    }
    return *stream;
}

std::optional<core::UserId> publisher_of(std::string_view identity,
                                         const core::LiveStreamId& stream) {
    const auto slash = identity.find('/');
    if (slash == std::string_view::npos || identity.substr(slash + 1) != stream.to_string()) {
        return std::nullopt;
    }
    const auto owner = core::UserId::parse(identity.substr(0, slash));
    if (!owner) {
        return std::nullopt;
    }
    return *owner;
}

} // namespace gateway
