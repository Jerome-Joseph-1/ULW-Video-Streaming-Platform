#pragma once

#include "os/unique_fd.hpp"

#include <cstdint>
#include <expected>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>

namespace live {

// The socket a publisher connects to. One publisher makes one stream: the first connection is
// accepted, the listening socket closes with it, and a second publisher is refused.
class IngestListener {
public:
    [[nodiscard]] static std::expected<IngestListener, std::string> bind(std::string_view host,
                                                                         std::uint16_t port);

    // The port bound, which is the requested one unless that was 0.
    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }

    // Waits for the publisher and returns its connection, an ordinary blocking descriptor to
    // hand to the remuxer as its input; an empty descriptor when `stop` fires first.
    [[nodiscard]] std::expected<os::UniqueFd, std::string> accept(const std::stop_token& stop);

private:
    IngestListener(os::UniqueFd fd, std::uint16_t port) noexcept
        : fd_(std::move(fd)), port_(port) {}

    os::UniqueFd fd_;
    std::uint16_t port_;
};

} // namespace live
