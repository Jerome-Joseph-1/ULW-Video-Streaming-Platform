#include "http/request.hpp"

#include "ascii.hpp"

#include <optional>
#include <span>
#include <string_view>

namespace http {

std::optional<std::string_view> find_header(std::span<const HeaderField> headers,
                                            std::string_view name) noexcept {
    for (const HeaderField& field : headers) {
        if (detail::iequals(field.name, name)) {
            return field.value;
        }
    }
    return std::nullopt;
}

} // namespace http
