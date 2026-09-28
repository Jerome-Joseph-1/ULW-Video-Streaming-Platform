#pragma once

#include <cstddef>
#include <string_view>

namespace ulw::test {

// In an executable linked with test_stalled_resolver, a lookup of any name under this domain
// blocks inside getaddrinfo() until release_stalled_lookups(), then fails. Other names resolve
// as usual. ".test" is reserved (RFC 2606), so no real name can collide with it.
inline constexpr std::string_view kStalledDomain = "stalled.test";

// Lookups blocked right now.
[[nodiscard]] std::size_t stalled_lookups();
// Lets every lookup blocked right now fail; later ones block again.
void release_stalled_lookups();

} // namespace ulw::test
