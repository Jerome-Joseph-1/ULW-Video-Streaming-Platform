#pragma once

#include <cstdio>
#include <exception>
#include <format>
#include <iterator>
#include <print>
#include <string>
#include <utility>

namespace chat {

// One JSON object per line on stdout, flushed at once, so a supervisor reading the pipe sees
// each event when it happens. `fields` is the object's members without the braces; every value
// passed in is a node, room or user id, or a number, none of which needs escaping.
template <class... Args>
void log_event(std::format_string<Args...> fields, Args&&... args) noexcept {
    try {
        std::string line = "{";
        std::format_to(std::back_inserter(line), fields, std::forward<Args>(args)...);
        line += '}';
        std::println(stdout, "{}", line);
        static_cast<void>(std::fflush(stdout));
    } catch (const std::exception&) {
        // A log line is never worth the loop; say that one went missing where it can be seen.
        static_cast<void>(std::fputs("chat_server: log line dropped\n", stderr));
    }
}

} // namespace chat
