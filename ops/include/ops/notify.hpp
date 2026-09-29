#pragma once

#include "core/util/time.hpp"
#include "os/unique_fd.hpp"

#include "ops/settings.hpp"

#include <expected>
#include <optional>
#include <string>
#include <string_view>

namespace ops {

// The service manager's readiness and watchdog protocol (sd_notify(3)): one datagram of
// newline-separated assignments to the AF_UNIX socket named by NOTIFY_SOCKET. That is all
// libsystemd does for these messages; linking it for one sendto() would add a dependency to
// every image, and the protocol is stable and documented.
class Notifier {
public:
    // nullopt when NOTIFY_SOCKET is unset, as it is outside a Type=notify unit; an error when
    // it is set but unusable.
    [[nodiscard]] static std::expected<std::optional<Notifier>, int> from_env(const Lookup& env,
                                                                              int own_pid);

    // Sends one message; never blocks. A manager that is not reading loses it.
    [[nodiscard]] std::expected<void, int> send(std::string_view message) const noexcept;
    void ready() const noexcept;
    void stopping() const noexcept;
    void watchdog() const noexcept;

    // How often to send WATCHDOG=1: half the manager's WATCHDOG_USEC, so one late tick still
    // lands in time. nullopt when the manager keeps no watchdog for this process.
    [[nodiscard]] std::optional<core::Millis> watchdog_interval() const noexcept {
        return watchdog_interval_;
    }

private:
    void post(std::string_view message) const noexcept;

    Notifier(os::UniqueFd fd, std::string address, std::optional<core::Millis> watchdog) noexcept
        : fd_(std::move(fd)), address_(std::move(address)), watchdog_interval_(watchdog) {}

    os::UniqueFd fd_;
    // A path, or an abstract name with its leading '@' turned into the NUL the kernel wants.
    std::string address_;
    std::optional<core::Millis> watchdog_interval_;
};

} // namespace ops
