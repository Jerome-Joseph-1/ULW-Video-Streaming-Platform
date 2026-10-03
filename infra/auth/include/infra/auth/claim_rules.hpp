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
    // ULW_LIVE_BROADCASTER_CLAIM: a token may start a live stream only when its claim of this
    // name is this value (a string equal to it, an array holding it, or `true` for "true").
    // No name: every token may. The initializers let designated initializers that predate the
    // fields leave them out.
    // NOLINTBEGIN(readability-redundant-member-init)
    std::string broadcaster_claim{};
    std::string broadcaster_value{};
    // NOLINTEND(readability-redundant-member-init)
};

} // namespace infra::auth
