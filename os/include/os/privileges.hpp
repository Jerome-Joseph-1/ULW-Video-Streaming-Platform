#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

namespace os {

struct Identity {
    std::uint32_t uid;
    std::uint32_t gid;
};

[[nodiscard]] bool is_root() noexcept;

// A user's id and primary group from the passwd database.
[[nodiscard]] std::expected<Identity, std::string> resolve_user(std::string_view name);

// Gives up root for good: supplementary groups first, then the group, then the user, which
// has to come last because setgroups and setgid need the capability setuid discards, and then
// no_new_privs, so no later exec (of a setuid-root binary, say) can raise privileges again.
// Then it checks the result rather than trusting the calls: every real, effective and saved id
// is the target, no supplementary group is left, no capability is held, no_new_privs is set,
// and setuid(0) and setgid(0) are refused. Any failure leaves the process in an unknown state,
// so the caller exits.
// Refuses a target with a zero uid or gid. Call it before any thread exists that should keep
// root: glibc applies each id change to all threads, so none survives with the old ids.
[[nodiscard]] std::expected<void, std::string> drop_privileges(Identity target);

[[nodiscard]] std::expected<void, std::string> drop_to_user(std::string_view name);

} // namespace os
