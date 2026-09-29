#include "ops/notify.hpp"

#include "core/util/parse.hpp"

#include <sys/socket.h>
#include <sys/un.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstring>

namespace ops {

std::expected<std::optional<Notifier>, int> Notifier::from_env(const Lookup& env, int own_pid) {
    const auto socket_name = env("NOTIFY_SOCKET");
    if (!socket_name || socket_name->empty()) {
        return std::nullopt;
    }
    std::string address = *socket_name;
    if (address.front() == '@') {
        address.front() = '\0';
    } else if (address.front() != '/') {
        return std::unexpected(EAFNOSUPPORT);
    }
    // sun_path holds 108 bytes; a path needs its terminating NUL, an abstract name does not.
    if (address.size() >= sizeof(sockaddr_un::sun_path)) {
        return std::unexpected(ENAMETOOLONG);
    }
    os::UniqueFd fd{::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0)};
    if (!fd) {
        return std::unexpected(errno);
    }
    std::optional<core::Millis> interval;
    const auto usec = env("WATCHDOG_USEC");
    const auto pid = env("WATCHDOG_PID");
    const bool ours = !pid || core::parse_integer<int>(*pid) == own_pid;
    if (usec && ours) {
        const auto micros = core::parse_integer<std::uint64_t>(*usec);
        if (!micros || *micros == 0) {
            return std::unexpected(EINVAL);
        }
        const auto period = std::chrono::duration_cast<core::Millis>(
            std::chrono::microseconds(static_cast<std::int64_t>(*micros)));
        interval = std::max(period / 2, core::Millis{1});
    }
    return Notifier(std::move(fd), std::move(address), interval);
}

std::expected<void, int> Notifier::send(std::string_view message) const noexcept {
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::memcpy(static_cast<void*>(addr.sun_path), address_.data(), address_.size());
    const auto len = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + address_.size() +
                                            (address_.front() == '\0' ? 0 : 1));
    // sendto() takes every address family through the generic header.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    const ssize_t n = ::sendto(fd_.get(), message.data(), message.size(), MSG_NOSIGNAL,
                               reinterpret_cast<const sockaddr*>(&addr), len);
    if (n < 0) {
        return std::unexpected(errno);
    }
    return {};
}

void Notifier::ready() const noexcept {
    static_cast<void>(send("READY=1"));
}

void Notifier::stopping() const noexcept {
    static_cast<void>(send("STOPPING=1"));
}

void Notifier::watchdog() const noexcept {
    static_cast<void>(send("WATCHDOG=1"));
}

} // namespace ops
