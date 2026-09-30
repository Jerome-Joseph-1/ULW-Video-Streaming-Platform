#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

namespace ops {

enum class RootStep : std::uint8_t { NotRoot, StayedRoot, Dropped };

struct RootRefusal {
    // True when the configuration is at fault (exit 2); false when the drop itself failed.
    bool configuration = true;
    std::string source;
    std::string reason;
};

// ULW_ALLOW_ROOT's value: "0" or "1", unset meaning "0".
[[nodiscard]] std::optional<bool> parse_allow_root(std::optional<std::string_view> text) noexcept;

// What every service does about being root, once its configuration is read and before any
// thread, job or client exists: nothing when it is not root; become `user` when one is named;
// stay root only when `allow_root`; otherwise refuse, as a configuration error. A listening
// socket a service needs root for must be bound before this.
[[nodiscard]] std::expected<RootStep, RootRefusal> leave_root(std::string_view user,
                                                              bool allow_root);

} // namespace ops
