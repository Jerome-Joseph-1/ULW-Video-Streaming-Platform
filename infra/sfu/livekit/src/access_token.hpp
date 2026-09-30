#pragma once

#include "core/util/time.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace infra::sfu::livekit::detail {

struct ApiKey {
    std::string id;
    std::string secret;
};

// Each is the least a caller needs; a token never carries a permission its use does not.
enum class Permission : std::uint8_t {
    // Join `room` as `identity`, publishing and subscribing.
    JoinRoom,
    // Join `room` as `identity` to publish camera and microphone only: no subscribing and no
    // data, which is all a WHIP ingest does.
    PublishToRoom,
    // RoomService.CreateRoom and DeleteRoom, which LiveKit guards with one permission.
    CreateRooms,
    // Egress.StartParticipantEgress: LiveKit's recorder joins `room` and sends one participant
    // on.
    RecordRoom,
};

struct Grant {
    Permission permission;
    std::string_view room;
    // The participant's identity; LiveKit reads it from `sub`. Empty for the server's own calls.
    std::string_view identity;
};

struct MintedToken {
    std::string jwt;
    core::WallTime expires_at;
};

// An HS256 JWT in LiveKit's access token shape (the `video` grant). JWT times are whole
// seconds, so `now` is truncated and the token expires `ttl` after that. nullopt only when
// OpenSSL fails.
[[nodiscard]] std::optional<MintedToken> mint_token(const ApiKey& key, const Grant& grant,
                                                    core::WallTime now, core::Seconds ttl);

} // namespace infra::sfu::livekit::detail
