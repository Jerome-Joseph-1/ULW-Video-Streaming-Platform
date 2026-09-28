#pragma once

#include "core/ports/clock.hpp"
#include "net/reactor.hpp"

#include <expected>
#include <memory>
#include <optional>
#include <string_view>

namespace net {

enum class ReactorKind { IoUring, Epoll };

[[nodiscard]] std::optional<ReactorKind> parse_reactor_kind(std::string_view name) noexcept;
[[nodiscard]] std::string_view to_string(ReactorKind kind) noexcept;

// `max_fds` sizes the descriptor-indexed slot table; descriptors at or above it are refused.
// An io_uring reactor must be created on the thread that will run it.
[[nodiscard]] std::expected<std::unique_ptr<IReactor>, int>
make_reactor(ReactorKind kind, core::ports::IClock& clock, std::size_t max_fds);

struct ReactorChoice {
    std::unique_ptr<IReactor> reactor;
    ReactorKind kind;
    // Set when io_uring was requested but unavailable; the errno explains why.
    std::optional<int> fell_back_from_io_uring;
};

// io_uring falls back to epoll when the kernel disables it (kernel.io_uring_disabled, or a
// seccomp profile that rejects io_uring_setup, as Docker's default does).
[[nodiscard]] std::expected<ReactorChoice, int>
make_reactor_with_fallback(ReactorKind preferred, core::ports::IClock& clock, std::size_t max_fds);

} // namespace net
