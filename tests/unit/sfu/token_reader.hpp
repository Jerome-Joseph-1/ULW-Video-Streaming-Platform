#pragma once

#include "core/util/json.hpp"
#include "infra/auth/base64url.hpp"

#include <array>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <optional>
#include <string>
#include <string_view>

namespace ulw::test {

// A LiveKit token taken apart the way the server does: the signature checked with the secret,
// then the header and claims parsed.
struct ReadToken {
    core::json::Value header;
    core::json::Value claims;
};

inline std::optional<ReadToken> read_token(std::string_view jwt, std::string_view secret) {
    const auto first = jwt.find('.');
    const auto last = jwt.rfind('.');
    if (first == std::string_view::npos || first == last) {
        return std::nullopt;
    }
    const std::string_view signing_input = jwt.substr(0, last);
    const auto signature = infra::auth::decode_base64url(jwt.substr(last + 1));
    std::array<unsigned char, EVP_MAX_MD_SIZE> mac{};
    unsigned int mac_size = 0;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    const auto* input = reinterpret_cast<const unsigned char*>(signing_input.data());
    if (!signature ||
        HMAC(EVP_sha256(), secret.data(), static_cast<int>(secret.size()), input,
             signing_input.size(), mac.data(), &mac_size) == nullptr ||
        signature->size() != mac_size ||
        CRYPTO_memcmp(signature->data(), mac.data(), mac_size) != 0) {
        return std::nullopt;
    }
    const auto header = infra::auth::decode_base64url(jwt.substr(0, first));
    const auto claims = infra::auth::decode_base64url(jwt.substr(first + 1, last - first - 1));
    if (!header || !claims) {
        return std::nullopt;
    }
    auto h = core::json::parse(*header);
    auto c = core::json::parse(*claims);
    if (!h || !c) {
        return std::nullopt;
    }
    return ReadToken{.header = std::move(*h), .claims = std::move(*c)};
}

inline std::optional<std::string_view> string_at(const core::json::Value& v, std::string_view a,
                                                 std::string_view b = {}) {
    const core::json::Value* at = v.find(a);
    if (at != nullptr && !b.empty()) {
        at = at->find(b);
    }
    return at == nullptr ? std::nullopt : at->as_string();
}

inline std::optional<bool> bool_at(const core::json::Value& v, std::string_view a,
                                   std::string_view b) {
    const core::json::Value* outer = v.find(a);
    const core::json::Value* at = outer == nullptr ? nullptr : outer->find(b);
    return at == nullptr ? std::nullopt : at->as_bool();
}

} // namespace ulw::test
