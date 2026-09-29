#pragma once

#include "codec/sdp/session.hpp"

#include <cstdint>
#include <optional>
#include <string_view>

namespace codec::sdp::detail {

enum class Level : std::uint8_t { Session, Media };

// The typed attribute for a=<name>[:<value>], an UnknownAttribute for a name not typed here,
// or nothing when a typed attribute's value breaks its grammar.
[[nodiscard]] std::optional<AttributeValue> parse_attribute(std::string_view name,
                                                            std::optional<std::string_view> value);

[[nodiscard]] bool allowed_at(const AttributeValue& value, Level level);

[[nodiscard]] std::string_view attribute_name(const AttributeValue& value);

[[nodiscard]] std::string_view direction_name(Direction d) noexcept;
[[nodiscard]] std::string_view setup_name(SetupRole role) noexcept;
[[nodiscard]] std::string_view rid_direction_name(RidDirection d) noexcept;

} // namespace codec::sdp::detail
