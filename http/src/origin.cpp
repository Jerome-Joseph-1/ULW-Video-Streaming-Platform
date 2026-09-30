#include "http/origin.hpp"

#include <algorithm>

namespace http {

bool is_origin(std::string_view origin) noexcept {
    std::string_view rest;
    if (origin.starts_with("https://")) {
        rest = origin.substr(8);
    } else if (origin.starts_with("http://")) {
        rest = origin.substr(7);
    } else {
        return false;
    }
    return !rest.empty() && rest.find_first_of("/?#@ ") == std::string_view::npos &&
           std::ranges::none_of(rest, [](char c) { return c >= 'A' && c <= 'Z'; });
}

std::optional<std::vector<std::string>> parse_origin_list(std::string_view list) {
    std::vector<std::string> out;
    while (!list.empty()) {
        const std::size_t comma = list.find(',');
        const std::string_view origin = list.substr(0, comma);
        if (!is_origin(origin)) {
            return std::nullopt;
        }
        out.emplace_back(origin);
        list = comma == std::string_view::npos ? std::string_view{} : list.substr(comma + 1);
    }
    return out;
}

bool origin_allowed(std::span<const std::string> allowed, std::string_view origin) noexcept {
    return std::ranges::find(allowed, origin) != allowed.end();
}

} // namespace http
