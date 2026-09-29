#pragma once

#include <cstdint>
#include <optional>

namespace ops {

// Both read /proc, which never waits on a device but is still a system call per entry: call
// them from a thread that may block, not from an event loop.
[[nodiscard]] std::optional<std::uint64_t> open_descriptors() noexcept;
[[nodiscard]] std::optional<std::uint64_t> resident_bytes() noexcept;

} // namespace ops
