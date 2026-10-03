#pragma once

#include <string>

namespace infra::auth {

// What a token must say about who issued it and whom it is for (JWT_ISSUER, JWT_AUDIENCE), and
// which claim names its user (ULW_JWT_SUBJECT_CLAIM).
struct ClaimRules {
    std::string issuer;
    std::string audience;
    // `sub` unless the identity provider puts the user's identifier in a claim of its own. A
    // token without this claim is refused as having no subject; no other claim stands in.
    std::string subject_claim = "sub";
};

} // namespace infra::auth
