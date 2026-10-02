#pragma once

#include <sys/types.h>

#include <array>
#include <optional>
#include <pwd.h>

namespace ulw::test {

// The uid of the user `name`, or nullopt when the host has no such user. getpwnam_r rather
// than getpwnam: a test may run beside threads of its own, and getpwnam's result lives in a
// buffer every caller shares.
inline std::optional<uid_t> user_id(const char* name) {
    passwd entry{};
    std::array<char, 4096> buffer{};
    passwd* found = nullptr;
    if (::getpwnam_r(name, &entry, buffer.data(), buffer.size(), &found) != 0 || found == nullptr) {
        return std::nullopt;
    }
    return found->pw_uid;
}

} // namespace ulw::test
