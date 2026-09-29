#include "os/privileges.hpp"

#include <linux/capability.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/types.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <format>
#include <grp.h>
#include <pwd.h>
#include <system_error>
#include <unistd.h>
#include <vector>

namespace os {

namespace {

std::string describe(std::string_view step, int error) {
    return std::format("{}: {}", step, std::generic_category().message(error));
}

bool holds_capabilities() {
    __user_cap_header_struct header{.version = _LINUX_CAPABILITY_VERSION_3, .pid = 0};
    std::array<__user_cap_data_struct, _LINUX_CAPABILITY_U32S_3> sets{};
    if (::syscall(SYS_capget, &header, sets.data()) != 0) {
        return true;
    }
    return std::ranges::any_of(
        sets, [](const auto& word) { return word.effective != 0 || word.permitted != 0; });
}

std::expected<void, std::string> verify(Identity target) {
    uid_t real = 0;
    uid_t effective = 0;
    uid_t saved = 0;
    gid_t real_group = 0;
    gid_t effective_group = 0;
    gid_t saved_group = 0;
    if (::getresuid(&real, &effective, &saved) != 0 ||
        ::getresgid(&real_group, &effective_group, &saved_group) != 0) {
        return std::unexpected(describe("read back ids", errno));
    }
    if (real != target.uid || effective != target.uid || saved != target.uid ||
        real_group != target.gid || effective_group != target.gid || saved_group != target.gid) {
        return std::unexpected("ids differ from the target after the drop");
    }
    if (::getgroups(0, nullptr) != 0) {
        return std::unexpected("supplementary groups remain after the drop");
    }
    if (holds_capabilities()) {
        return std::unexpected("capabilities remain after the drop");
    }
    if (::prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0) != 1) {
        return std::unexpected("no_new_privs is not set after the drop");
    }
    // The saved ids are what would let setuid(0) succeed; a refusal proves there is no way back.
    if (::setuid(0) == 0 || ::setgid(0) == 0) {
        return std::unexpected("root can be regained after the drop");
    }
    return {};
}

} // namespace

bool is_root() noexcept {
    return ::geteuid() == 0;
}

std::expected<Identity, std::string> resolve_user(std::string_view name) {
    const std::string user(name);
    // The suggested size is a hint that entries can exceed; ERANGE asks for more.
    const long hint = ::sysconf(_SC_GETPW_R_SIZE_MAX);
    std::vector<char> buffer(hint > 0 ? static_cast<std::size_t>(hint) : std::size_t{1} << 14U);
    constexpr std::size_t kMaxBuffer = std::size_t{1} << 20U;
    while (true) {
        passwd entry{};
        passwd* found = nullptr;
        const int rc = ::getpwnam_r(user.c_str(), &entry, buffer.data(), buffer.size(), &found);
        if (rc == ERANGE && buffer.size() < kMaxBuffer) {
            buffer.resize(buffer.size() * 2);
            continue;
        }
        if (rc != 0) {
            return std::unexpected(describe("look up user", rc));
        }
        if (found == nullptr) {
            return std::unexpected(std::format("no such user: {}", user));
        }
        return Identity{.uid = entry.pw_uid, .gid = entry.pw_gid};
    }
}

std::expected<void, std::string> drop_privileges(Identity target) {
    if (target.uid == 0 || target.gid == 0) {
        return std::unexpected("refusing to drop to a root uid or gid");
    }
    if (::setgroups(0, nullptr) != 0) {
        return std::unexpected(describe("setgroups", errno));
    }
    if (::setgid(target.gid) != 0) {
        return std::unexpected(describe("setgid", errno));
    }
    if (::setuid(target.uid) != 0) {
        return std::unexpected(describe("setuid", errno));
    }
    // Without it, exec of a setuid-root program (su and mount ship in the runtime image) would
    // hand root back, and nothing about our own ids would show it.
    if (::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
        return std::unexpected(describe("prctl no_new_privs", errno));
    }
    return verify(target);
}

std::expected<void, std::string> drop_to_user(std::string_view name) {
    const auto identity = resolve_user(name);
    if (!identity) {
        return std::unexpected(identity.error());
    }
    return drop_privileges(*identity);
}

} // namespace os
