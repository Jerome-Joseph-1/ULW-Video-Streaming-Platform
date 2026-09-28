#include "core/version.hpp"

#include "ulw/build_info.inc"

namespace core {

BuildInfo build_info() noexcept {
    return {.version = ULW_VERSION, .git_sha = ULW_GIT_SHA};
}

} // namespace core
