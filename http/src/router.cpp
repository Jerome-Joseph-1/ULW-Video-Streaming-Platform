#include "http/router.hpp"

#include <cstddef>
#include <span>
#include <string_view>

namespace http::detail {

bool match_path(std::string_view pattern, std::string_view path, PathParams& params) noexcept {
    if (path.empty() || path.front() != '/') {
        return false;
    }
    const std::span<std::string_view> out{params};
    std::size_t captured = 0;
    // Both start with '/'; each pass consumes one segment of each.
    while (true) {
        pattern.remove_prefix(1);
        path.remove_prefix(1);
        const std::size_t pattern_end = pattern.find('/');
        const std::size_t path_end = path.find('/');
        const std::string_view expected = pattern.substr(0, pattern_end);
        const std::string_view segment = path.substr(0, path_end);
        if (is_param(expected)) {
            if (segment.empty()) {
                return false;
            }
            out[captured++] = segment;
        } else if (expected != segment) {
            return false;
        }
        if (pattern_end == std::string_view::npos || path_end == std::string_view::npos) {
            return pattern_end == path_end;
        }
        pattern.remove_prefix(pattern_end);
        path.remove_prefix(path_end);
    }
}

} // namespace http::detail
