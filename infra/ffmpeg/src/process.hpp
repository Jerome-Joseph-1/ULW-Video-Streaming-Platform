#pragma once

#include "core/ports/clock.hpp"
#include "core/util/time.hpp"

#include "command.hpp"
#include "exit_code.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace infra::ffmpeg {

// Where a child runs: through the ulw_sandbox helper, with exactly this environment.
struct Sandbox {
    std::filesystem::path helper;
    // "NAME=value" entries. Nothing of the worker's own environment is passed on: it holds
    // the database password and the storage keys.
    std::vector<std::string> environment;
};

struct Limits {
    // The only directory the child may write.
    std::filesystem::path writable;
    std::uint64_t address_space_bytes = 0;
    core::Seconds cpu{};
    core::Millis wall{};
    // The most any file the child writes may grow to; 0 sets no limit.
    std::uint64_t file_size_bytes = 0;
};

struct ChildExit {
    // As a shell reports it: 128 + the signal number for a signalled child.
    int exit_code = 0;
    // The signal that killed the child, or 0 when it exited. Only this tells a signal from an
    // exit code above 128, which ffmpeg uses for its own failures.
    int signal = 0;
    Ending ending = Ending::Exited;
    core::Millis wall{};
    std::uint64_t peak_rss_kib = 0;
    // The last few KiB the child wrote to stderr, and how much it wrote in all.
    std::string stderr_tail;
    std::size_t stderr_bytes = 0;
};

// After SIGTERM, how long a child gets to exit before SIGKILL.
inline constexpr core::Millis kTerminationGrace{5000};

// Starts `args` in the sandbox, hands whatever it writes to stdout to `on_stdout` as it
// arrives, and waits for it to exit. Its stdin is /dev/null, or the open descriptor `input`
// when that is not -1: the only way media from the network reaches a child that has no network
// of its own. At the deadline, or once `stop` fires, the child gets SIGTERM and,
// kTerminationGrace later, SIGKILL. Fails only when the child could not be started at all.
[[nodiscard]] std::expected<ChildExit, std::string>
run_sandboxed(const Sandbox& sandbox, const Limits& limits, const Args& args,
              const core::ports::IClock& clock,
              const std::function<void(std::string_view)>& on_stdout, const std::stop_token& stop,
              int input = -1);

} // namespace infra::ffmpeg
