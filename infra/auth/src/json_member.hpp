#pragma once

#include "core/util/json.hpp"

#include <optional>
#include <string_view>

namespace infra::auth::detail {

// nullopt when the member is absent or is not a string.
[[nodiscard]] inline std::optional<std::string_view> string_member(const core::json::Value& object,
                                                                   std::string_view name) noexcept {
    const core::json::Value* v = object.find(name);
    return v == nullptr ? std::nullopt : v->as_string();
}

} // namespace infra::auth::detail
