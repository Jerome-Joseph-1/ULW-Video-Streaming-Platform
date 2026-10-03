#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <string_view>

namespace ops {

// Both read /proc, which never waits on a device but is still a system call per entry: call
// them from a thread that may block, not from an event loop.
[[nodiscard]] std::optional<std::uint64_t> open_descriptors() noexcept;
[[nodiscard]] std::optional<std::uint64_t> resident_bytes() noexcept;

// No core file, and a process no other process of its user may read or attach to: RLIMIT_CORE
// 0, soft and hard, and PR_SET_DUMPABLE 0. The environment, and the memory of any process that
// has read it, holds the database password, the store keys and live bearer tokens, which a
// crash would otherwise write to disk and /proc/<pid>/environ would show every process of the
// same user. The limit is inherited by children; the flag is reset by exec, and by a change of
// uid, which ops::leave_root puts back. Call it first, before the configuration is read. The
// error is the errno of the call that failed.
[[nodiscard]] std::expected<void, int> disable_core_dumps() noexcept;

// The version of the jemalloc this process allocates with, as jemalloc reports it, or empty when
// it allocates with glibc's malloc: the services link jemalloc (ADR-0081) and log which they got.
[[nodiscard]] std::string_view jemalloc_version() noexcept;

} // namespace ops
