#pragma once

#include "http/method.hpp"
#include "http/status.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <expected>
#include <span>
#include <stdexcept>
#include <string_view>

namespace http {

// Matches are returned by value; the deepest route, .../videos/{id}/{rendition}/index.m3u8,
// uses two and four views still fit in a cache line.
inline constexpr std::size_t kMaxPathParams = 4;

using PathParams = std::array<std::string_view, kMaxPathParams>;

// `pattern` is '/'-separated literal segments and whole-segment {name} parameters.
template <typename Id> struct Route {
    Method method{};
    std::string_view pattern;
    Id id{};
};

template <typename Id> struct RouteMatch {
    Id id;
    // In pattern order, as views into the request target; the rest are empty.
    PathParams params;
};

struct RouteMiss {
    // NotFound, MethodNotAllowed or NotImplemented.
    Status status;
    // What a 405's Allow field must list.
    MethodSet allow;
};

namespace detail {

[[nodiscard]] constexpr bool is_param(std::string_view segment) noexcept {
    return segment.size() > 2 && segment.front() == '{' && segment.back() == '}';
}

[[nodiscard]] constexpr bool is_valid_pattern(std::string_view pattern) noexcept {
    if (pattern == "/") {
        return true;
    }
    if (pattern.empty() || pattern.front() != '/') {
        return false;
    }
    std::size_t params = 0;
    std::size_t begin = 1;
    while (begin <= pattern.size()) {
        const std::size_t end = std::min(pattern.find('/', begin), pattern.size());
        const std::string_view segment = pattern.substr(begin, end - begin);
        const bool param = is_param(segment);
        const std::string_view text = param ? segment.substr(1, segment.size() - 2) : segment;
        if (text.empty() || text.find_first_of("{}?#") != std::string_view::npos) {
            return false;
        }
        params += param ? 1 : 0;
        begin = end + 1;
    }
    return params <= kMaxPathParams;
}

// Fills `params` as it goes, so it is only meaningful when this returns true.
[[nodiscard]] bool match_path(std::string_view pattern, std::string_view path,
                              PathParams& params) noexcept;

} // namespace detail

// Matches the path of a request target against a fixed table: no allocation, no decoding and
// no normalisation, so ".." or "%2F" reach the handler as the literal id they claim to be and
// are refused there by id validation.
template <typename Id> class Router {
public:
    // consteval: a malformed pattern fails the build instead of becoming a route that never
    // matches.
    consteval explicit Router(std::span<const Route<Id>> routes) : routes_(routes) {
        for (const Route<Id>& route : routes) {
            if (!detail::is_valid_pattern(route.pattern)) {
                throw std::invalid_argument("malformed route pattern");
            }
        }
    }

    [[nodiscard]] std::expected<RouteMatch<Id>, RouteMiss>
    match(Method method, std::string_view target) const noexcept {
        if (method == Method::Other) {
            return std::unexpected(RouteMiss{.status = Status::NotImplemented, .allow = {}});
        }
        const std::string_view path = target.substr(0, target.find_first_of("?#"));
        MethodSet allow;
        for (const Route<Id>& route : routes_) {
            PathParams params{};
            if (!detail::match_path(route.pattern, path, params)) {
                continue;
            }
            if (route.method == method) {
                return RouteMatch<Id>{.id = route.id, .params = params};
            }
            allow.add(route.method);
        }
        if (allow.empty()) {
            return std::unexpected(RouteMiss{.status = Status::NotFound, .allow = {}});
        }
        return std::unexpected(RouteMiss{.status = Status::MethodNotAllowed, .allow = allow});
    }

private:
    std::span<const Route<Id>> routes_;
};

} // namespace http
