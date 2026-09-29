#pragma once

#include "infra/auth/claim_rules.hpp"

#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace ulw::test {

inline const infra::auth::ClaimRules kTestRules{.issuer = "https://id.askedin.test",
                                                .audience = "askedin-platform"};

// A NumericDate `offset_seconds` from the FakeClock's starting wall time.
[[nodiscard]] std::string numeric_date(std::int64_t offset_seconds);

// A claim replaced by raw JSON text, or removed when nullopt.
using ClaimChange = std::pair<std::string_view, std::optional<std::string>>;

// Claims that pass kTestRules at the FakeClock's starting time, valid for an hour.
[[nodiscard]] std::string test_payload(std::initializer_list<ClaimChange> changes = {});

} // namespace ulw::test
