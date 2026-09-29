#pragma once

#include <cstdio>
#include <format>
#include <print>
#include <utility>

namespace worker {

// One line per event on stdout, flushed at once: a supervisor reading the pipe must see a line
// when it happens, not when a buffer fills, and a killed process loses nothing it logged.
template <class... Args> void log(std::format_string<Args...> format, Args&&... args) {
    std::println(stdout, "transcode_worker: {}", std::format(format, std::forward<Args>(args)...));
    static_cast<void>(std::fflush(stdout));
}

} // namespace worker
