#pragma once

#include <string_view>

namespace core {

struct BuildInfo {
    std::string_view version;
    std::string_view git_sha;
};

[[nodiscard]] BuildInfo build_info() noexcept;

} // namespace core
