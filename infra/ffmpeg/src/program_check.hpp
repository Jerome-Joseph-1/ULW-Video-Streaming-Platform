// Whether ulw_sandbox may run a program as named (sandbox_main.cpp): an absolute path in normal
// form (no "." or ".." component, no doubled slash) to a regular file this process may execute,
// symbolic links followed. A bare name is never searched for on PATH; process.cpp resolves it.
// Header-only for the same reason as seccomp_filter.hpp: the helper links nothing that is
// instrumented, and the tests check the same function the helper calls.
#pragma once

#include <sys/stat.h>

#include <cerrno>
#include <expected>
#include <fcntl.h>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>

namespace infra::ffmpeg::sandbox {

struct ProgramRefusal {
    // No such file, a bare name included, which names none; otherwise the file is there but
    // may not be run.
    bool not_found = false;
    std::string reason;
};

[[nodiscard]] inline std::expected<void, ProgramRefusal> check_program(std::string_view program) {
    if (!program.contains('/')) {
        return std::unexpected(
            ProgramRefusal{.not_found = true, .reason = "not a path, and no PATH is searched"});
    }
    const std::filesystem::path path(program);
    if (!path.is_absolute()) {
        return std::unexpected(ProgramRefusal{.reason = "not an absolute path"});
    }
    for (const auto& part : path) {
        if (part == "..") {
            return std::unexpected(ProgramRefusal{.reason = "has a .. component"});
        }
    }
    if (path.lexically_normal() != path || program.contains("//")) {
        return std::unexpected(ProgramRefusal{.reason = "not in normal form"});
    }
    struct stat status {};
    if (::stat(path.c_str(), &status) != 0) {
        const int error = errno;
        return std::unexpected(ProgramRefusal{.not_found = error == ENOENT,
                                              .reason = std::generic_category().message(error)});
    }
    if (!S_ISREG(status.st_mode)) {
        return std::unexpected(ProgramRefusal{.reason = "not a regular file"});
    }
    if (::faccessat(AT_FDCWD, path.c_str(), X_OK, AT_EACCESS) != 0) {
        return std::unexpected(ProgramRefusal{.reason = std::generic_category().message(errno)});
    }
    return {};
}

} // namespace infra::ffmpeg::sandbox
