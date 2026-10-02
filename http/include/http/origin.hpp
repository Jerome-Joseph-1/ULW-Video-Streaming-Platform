#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace http {

// A browser sends Origin as scheme://host[:port], printable ASCII, lowercase, with no path, the
// port 1-65535 without leading zeros and never the scheme's default; anything else could never
// match one. Plain http is taken only for a
// loopback host (localhost, 127.0.0.1, [::1]): elsewhere the cookie would cross the network in
// the clear.
[[nodiscard]] bool is_origin(std::string_view origin) noexcept;

// A comma-separated list of is_origin() values, as ULW_ALLOWED_ORIGINS holds. nullopt when any
// entry is not one; "" is the empty list.
[[nodiscard]] std::optional<std::vector<std::string>> parse_origin_list(std::string_view list);

// Exact match: an allowed origin is compared byte for byte, as a browser writes it.
[[nodiscard]] bool origin_allowed(std::span<const std::string> allowed,
                                  std::string_view origin) noexcept;

} // namespace http
