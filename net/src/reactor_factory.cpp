#include "net/reactor_factory.hpp"

#include "epoll_reactor.hpp"
#include "uring_reactor.hpp"

#include <cerrno>
#include <fstream>
#include <string>

namespace net {

namespace {

// Kernels without the sysctl have no file, and io_uring stays enabled.
bool io_uring_disabled_by_sysctl() {
    std::ifstream in("/proc/sys/kernel/io_uring_disabled");
    std::string value;
    return std::getline(in, value) && detail::io_uring_disabled(value);
}

} // namespace

std::optional<ReactorKind> parse_reactor_kind(std::string_view name) noexcept {
    if (name == "io_uring") {
        return ReactorKind::IoUring;
    }
    if (name == "epoll") {
        return ReactorKind::Epoll;
    }
    return std::nullopt;
}

std::string_view to_string(ReactorKind kind) noexcept {
    switch (kind) {
    case ReactorKind::IoUring:
        return "io_uring";
    case ReactorKind::Epoll:
        return "epoll";
    }
    return "unknown";
}

std::expected<std::unique_ptr<IReactor>, int>
make_reactor(ReactorKind kind, core::ports::IClock& clock, std::size_t max_fds) {
    switch (kind) {
    case ReactorKind::IoUring: {
        if (io_uring_disabled_by_sysctl()) {
            return std::unexpected(EPERM);
        }
        auto r = detail::UringReactor::create(clock, max_fds);
        if (!r) {
            return std::unexpected(r.error());
        }
        return std::unique_ptr<IReactor>(std::move(*r));
    }
    case ReactorKind::Epoll: {
        auto r = detail::EpollReactor::create(clock, max_fds);
        if (!r) {
            return std::unexpected(r.error());
        }
        return std::unique_ptr<IReactor>(std::move(*r));
    }
    }
    return std::unexpected(EINVAL);
}

std::expected<ReactorChoice, int>
make_reactor_with_fallback(ReactorKind preferred, core::ports::IClock& clock, std::size_t max_fds) {
    auto first = make_reactor(preferred, clock, max_fds);
    if (first) {
        return ReactorChoice{.reactor = std::move(*first),
                             .kind = preferred,
                             .fell_back_from_io_uring = std::nullopt};
    }
    if (preferred != ReactorKind::IoUring) {
        return std::unexpected(first.error());
    }
    auto epoll = make_reactor(ReactorKind::Epoll, clock, max_fds);
    if (!epoll) {
        return std::unexpected(epoll.error());
    }
    return ReactorChoice{.reactor = std::move(*epoll),
                         .kind = ReactorKind::Epoll,
                         .fell_back_from_io_uring = first.error()};
}

} // namespace net
