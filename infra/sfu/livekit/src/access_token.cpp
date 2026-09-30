#include "access_token.hpp"

#include "core/util/json.hpp"
#include "infra/auth/base64url.hpp"

#include <array>
#include <chrono>
#include <climits>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <span>

namespace infra::sfu::livekit::detail {

namespace {

void append_video_grant(std::string& out, const Grant& grant) {
    out += R"("video":{)";
    switch (grant.permission) {
    case Permission::JoinRoom:
        out += R"("room":)";
        core::json::append_string(out, grant.room);
        out += R"(,"roomJoin":true,"canPublish":true,"canSubscribe":true)";
        break;
    case Permission::PublishToRoom:
        out += R"("room":)";
        core::json::append_string(out, grant.room);
        // A WHIP session's audio is the microphone source and its video the camera
        // (synthesizeAddTrackRequests, pkg/rtc/participant.go in LiveKit v1.13.7).
        out += R"(,"roomJoin":true,"canPublish":true,"canSubscribe":false,"canPublishData":false)";
        out += R"(,"canPublishSources":["camera","microphone"])";
        break;
    case Permission::CreateRooms:
        out += R"("roomCreate":true)";
        break;
    case Permission::RecordRoom:
        out += R"("roomRecord":true)";
        break;
    }
    out += '}';
}

} // namespace

std::optional<MintedToken> mint_token(const ApiKey& key, const Grant& grant, core::WallTime now,
                                      core::Seconds ttl) {
    const auto issued = std::chrono::floor<core::Seconds>(now);
    const auto expires = issued + ttl;

    std::string claims = R"({"iss":)";
    core::json::append_string(claims, key.id);
    if (!grant.identity.empty()) {
        claims += R"(,"sub":)";
        core::json::append_string(claims, grant.identity);
    }
    claims += R"(,"nbf":)";
    claims += std::to_string(issued.time_since_epoch().count());
    claims += R"(,"exp":)";
    claims += std::to_string(expires.time_since_epoch().count());
    claims += ',';
    append_video_grant(claims, grant);
    claims += '}';

    std::string jwt = auth::encode_base64url(R"({"alg":"HS256","typ":"JWT"})");
    jwt += '.';
    jwt += auth::encode_base64url(claims);

    if (key.secret.size() > INT_MAX) {
        return std::nullopt;
    }
    std::array<unsigned char, EVP_MAX_MD_SIZE> mac{};
    unsigned int mac_size = 0;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): OpenSSL takes bytes as uchar.
    const auto* input = reinterpret_cast<const unsigned char*>(jwt.data());
    if (HMAC(EVP_sha256(), key.secret.data(), static_cast<int>(key.secret.size()), input,
             jwt.size(), mac.data(), &mac_size) == nullptr) {
        return std::nullopt;
    }
    jwt += '.';
    jwt += auth::encode_base64url(std::span<const unsigned char>(mac.data(), mac_size));
    return MintedToken{.jwt = std::move(jwt), .expires_at = expires};
}

} // namespace infra::sfu::livekit::detail
