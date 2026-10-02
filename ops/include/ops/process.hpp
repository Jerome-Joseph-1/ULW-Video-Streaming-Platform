#pragma once

#include <cstdint>
#include <expected>
#include <optional>

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

// Hands every allocation of 16 KiB or more to mmap, and so back to the kernel when it is freed,
// instead of carving it from the heap, where a long-lived service's transient frame, message and
// parse buffers leave holes that glibc never returns (ADR-0078). Setting the threshold also
// stops glibc from raising it on its own, which is what lets those buffers back into the heap.
// Call it first, before anything is allocated in earnest. The error is EINVAL when the
// allocator refuses the setting, as a sanitizer's allocator does; the service runs on without it.
[[nodiscard]] std::expected<void, int> return_large_blocks() noexcept;

} // namespace ops
