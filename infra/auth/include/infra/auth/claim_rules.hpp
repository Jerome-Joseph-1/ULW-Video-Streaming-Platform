#pragma once

#include <string>

namespace infra::auth {

// What a token must say about who issued it and whom it is for (JWT_ISSUER, JWT_AUDIENCE).
struct ClaimRules {
    std::string issuer;
    std::string audience;
};

} // namespace infra::auth
