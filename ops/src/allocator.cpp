#include "ops/allocator.hpp"

#include "ops/process.hpp"

#include <array>
#include <format>
#include <malloc.h>
#include <string_view>
#include <utility>

namespace ops {

namespace {

// glibc's malloc variables (mallopt(3)): any of them set means the operator is tuning malloc.
constexpr std::array<std::string_view, 6> kMallocVariables{
    "MALLOC_ARENA_MAX",       "MALLOC_ARENA_TEST", "MALLOC_MMAP_THRESHOLD_",
    "MALLOC_TRIM_THRESHOLD_", "MALLOC_TOP_PAD_",   "MALLOC_MMAP_MAX_"};
constexpr std::string_view kTunables = "GLIBC_TUNABLES";
constexpr std::string_view kMallocTunable = "glibc.malloc.";

// The operator's malloc settings, "NAME=value" separated by spaces, or empty for none. An
// empty variable is no setting: glibc ignores it too. Of GLIBC_TUNABLES only the glibc.malloc.*
// entries count; it also carries tunables of other parts of glibc.
std::string operator_settings(const Lookup& env) {
    std::string found;
    const auto add = [&found](std::string_view entry) {
        if (!found.empty()) {
            found += ' ';
        }
        found += entry;
    };
    for (const std::string_view name : kMallocVariables) {
        if (const auto value = env(name); value && !value->empty()) {
            add(std::format("{}={}", name, *value));
        }
    }
    if (const auto tunables = env(kTunables)) {
        std::string_view rest = *tunables;
        while (!rest.empty()) {
            const std::size_t end = rest.find(':');
            const std::string_view entry = rest.substr(0, end);
            if (entry.starts_with(kMallocTunable)) {
                add(entry);
            }
            rest = end == std::string_view::npos ? std::string_view{} : rest.substr(end + 1);
        }
    }
    return found;
}

} // namespace

bool sanitizer_replaces_malloc() noexcept {
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__) || defined(__SANITIZE_HWADDRESS__)
    return true;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) ||                         \
    __has_feature(memory_sanitizer) || __has_feature(hwaddress_sanitizer)
    return true;
#else
    return false;
#endif
#else
    return false;
#endif
}

AllocatorSeams process_allocator(Lookup env) {
    return {.env = std::move(env),
            .jemalloc = !jemalloc_version().empty(),
            .sanitizer = sanitizer_replaces_malloc(),
            // mallopt takes malloc's own lock; it is "unsafe" only in racing other threads'
            // allocations for which settings they see, and it is called before any exists.
            // NOLINTNEXTLINE(concurrency-mt-unsafe)
            .mallopt = [](int option, int value) { return ::mallopt(option, value); }};
}

std::expected<AllocatorReport, std::string> tune_allocator(const MallocTuning& tuning,
                                                           const AllocatorSeams& seams) {
    if (seams.sanitizer) {
        return AllocatorReport{.choice = AllocatorChoice::Sanitizer, .description = "sanitizer"};
    }
    if (seams.jemalloc) {
        return AllocatorReport{.choice = AllocatorChoice::Jemalloc, .description = "jemalloc"};
    }
    if (auto set = operator_settings(seams.env); !set.empty()) {
        return AllocatorReport{.choice = AllocatorChoice::Operator,
                               .description = "glibc " + std::move(set)};
    }
    struct Step {
        int option;
        std::string_view name;
        int value;
    };
    // The arena limit first: it is the one that must precede every thread.
    const std::array<Step, 3> steps{
        {{.option = M_ARENA_MAX, .name = "M_ARENA_MAX", .value = tuning.arena_max},
         {.option = M_MMAP_THRESHOLD, .name = "M_MMAP_THRESHOLD", .value = tuning.mmap_threshold},
         {.option = M_TRIM_THRESHOLD, .name = "M_TRIM_THRESHOLD", .value = tuning.trim_threshold}}};
    for (const Step& step : steps) {
        if (seams.mallopt(step.option, step.value) != 1) {
            return std::unexpected(
                std::format("mallopt({}, {}) refused by glibc's malloc", step.name, step.value));
        }
    }
    return AllocatorReport{
        .choice = AllocatorChoice::Tuned,
        .description = std::format("glibc arena_max={} mmap_threshold={} trim_threshold={}",
                                   tuning.arena_max, tuning.mmap_threshold, tuning.trim_threshold)};
}

} // namespace ops
