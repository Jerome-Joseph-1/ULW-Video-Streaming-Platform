#pragma once

#include "os/unique_fd.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <stop_token>

namespace live {

// How media reaches a sandboxed ffmpeg, which has no network and no files of ours: it reads
// its stdin.
struct Pipe {
    os::UniqueFd read;
    os::UniqueFd write;
};

// Close-on-exec: the sandbox helper hands the child only the end it is given.
[[nodiscard]] std::optional<Pipe> make_pipe();

// Writes all of `bytes` to a pipe, waiting for room but never past `stop`: a reader that has
// gone leaves the pipe full for good.
[[nodiscard]] bool write_all(int fd, std::span<const std::byte> bytes, const std::stop_token& stop);

} // namespace live
