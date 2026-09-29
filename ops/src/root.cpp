#include "ops/root.hpp"

#include "os/privileges.hpp"

namespace ops {

std::optional<bool> parse_allow_root(std::optional<std::string_view> text) noexcept {
    if (!text || text->empty() || *text == "0") {
        return false;
    }
    if (*text == "1") {
        return true;
    }
    return std::nullopt;
}

std::expected<RootStep, RootRefusal> leave_root(std::string_view user, bool allow_root) {
    if (!os::is_root()) {
        return RootStep::NotRoot;
    }
    if (user.empty()) {
        if (allow_root) {
            return RootStep::StayedRoot;
        }
        return std::unexpected(RootRefusal{
            .configuration = true,
            .source = "ULW_RUN_AS_USER",
            .reason = "not set, and the process runs as root; set it, or ULW_ALLOW_ROOT=1"});
    }
    const auto identity = os::resolve_user(user);
    if (!identity) {
        return std::unexpected(RootRefusal{
            .configuration = true, .source = "ULW_RUN_AS_USER", .reason = identity.error()});
    }
    if (auto r = os::drop_privileges(*identity); !r) {
        return std::unexpected(
            RootRefusal{.configuration = false, .source = "drop privileges", .reason = r.error()});
    }
    return RootStep::Dropped;
}

} // namespace ops
