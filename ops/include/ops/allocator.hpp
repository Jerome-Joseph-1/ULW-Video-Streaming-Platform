#pragma once

#include "ops/settings.hpp"

#include <cstdint>
#include <expected>
#include <functional>
#include <string>

namespace ops {

// What glibc's malloc is set to before a service starts a thread (ADR-0094). One arena, so the
// worker threads' transient buffers do not each grow an arena of their own, and a fixed mmap
// threshold, so a buffer of 128 KiB or more is a mapping of its own, unmapped when freed,
// rather than pages the heap keeps touching. Fixing the threshold also stops glibc raising it
// after each such buffer is freed, which is what pulled them back into the heap. The trim
// threshold is the same, so the top of the heap goes back once that much is free there.
struct MallocTuning {
    int arena_max = 1;
    int mmap_threshold = 128 * 1024;
    int trim_threshold = 128 * 1024;
};

enum class AllocatorChoice : std::uint8_t {
    // glibc's malloc, with MallocTuning applied.
    Tuned,
    // glibc's malloc, left as the operator's environment set it (MALLOC_ARENA_MAX,
    // MALLOC_MMAP_THRESHOLD_, MALLOC_TRIM_THRESHOLD_ or their GLIBC_TUNABLES entries): applying
    // ours after glibc read those would quietly undo them.
    Operator,
    // jemalloc, linked or preloaded: mallopt would reach nothing.
    Jemalloc,
    // A sanitizer's allocator, which replaces malloc and ignores mallopt.
    Sanitizer,
};

struct AllocatorReport {
    AllocatorChoice choice = AllocatorChoice::Tuned;
    // For the startup line's "allocator" field: "glibc arena_max=1 mmap_threshold=131072
    // trim_threshold=131072", "glibc MALLOC_ARENA_MAX=2" (the operator's settings), "jemalloc"
    // or "sanitizer".
    std::string description;
};

// What tune_allocator looks at and calls; process_allocator() is the real process's.
struct AllocatorSeams {
    Lookup env;
    bool jemalloc = false;
    bool sanitizer = false;
    // ::mallopt: 1 on success, 0 when the value is refused.
    std::function<int(int option, int value)> mallopt;
};

// True when the build replaced malloc with a sanitizer's (ASan, TSan, MSan, HWASan, LSan).
[[nodiscard]] bool sanitizer_replaces_malloc() noexcept;

// This process: `env`, jemalloc found at run time (ops::jemalloc_version()), the build's
// sanitizer, and glibc's mallopt.
[[nodiscard]] AllocatorSeams process_allocator(Lookup env);

// Applies `tuning` with mallopt unless jemalloc or a sanitizer allocates, or the operator set
// one of these settings through the environment. Call it before any thread exists (the gateway
// calls it first in run(), which main calls), so every thread shares the one arena. The error
// names the option and value glibc refused. An allocator preloaded in place of glibc's that is
// not jemalloc (tcmalloc, say) answers mallopt itself, and may report Tuned for settings it
// ignores.
[[nodiscard]] std::expected<AllocatorReport, std::string>
tune_allocator(const MallocTuning& tuning, const AllocatorSeams& seams);

} // namespace ops
