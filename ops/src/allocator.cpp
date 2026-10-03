#include "ops/allocator.hpp"

#include "ops/process.hpp"

#include <array>
#include <format>
#include <malloc.h>
#include <string_view>
#include <utility>

namespace ops {

namespace {

// The settings tune_allocator makes, in both of glibc's spellings: an environment variable
// (mallopt(3)) and a GLIBC_TUNABLES entry. Either one set means the operator is choosing them,
// and the whole tuning is left out; the other malloc settings (perturb, check, tcache, top pad,
// mmap max) are not ours and are left to glibc whichever way they are spelt.
struct OperatorSetting {
    std::string_view variable;
    std::string_view tunable;
    // glibc 2.39 reads an empty MALLOC_MMAP_THRESHOLD_ or MALLOC_TRIM_THRESHOLD_ as 0
    // (elf/dl-tunables.c), so present is set; an empty MALLOC_ARENA_MAX is 0, below its
    // minimum of 1, and ignored.
    bool empty_counts;
};
constexpr std::array<OperatorSetting, 3> kOperatorSettings{{
    {.variable = "MALLOC_ARENA_MAX", .tunable = "glibc.malloc.arena_max", .empty_counts = false},
    {.variable = "MALLOC_MMAP_THRESHOLD_",
     .tunable = "glibc.malloc.mmap_threshold",
     .empty_counts = true},
    {.variable = "MALLOC_TRIM_THRESHOLD_",
     .tunable = "glibc.malloc.trim_threshold",
     .empty_counts = true},
}};
constexpr std::string_view kTunables = "GLIBC_TUNABLES";

// The operator's settings of what tune_allocator sets, "NAME=value" separated by spaces, or
// empty for none.
std::string operator_settings(const Lookup& env) {
    std::string found;
    const auto add = [&found](std::string_view entry) {
        if (!found.empty()) {
            found += ' ';
        }
        found += entry;
    };
    for (const OperatorSetting& setting : kOperatorSettings) {
        if (const auto value = env(setting.variable);
            value && (setting.empty_counts || !value->empty())) {
            add(std::format("{}={}", setting.variable, *value));
        }
    }
    if (const auto tunables = env(kTunables)) {
        std::string_view rest = *tunables;
        while (!rest.empty()) {
            const std::size_t end = rest.find(':');
            const std::string_view entry = rest.substr(0, end);
            const std::string_view name = entry.substr(0, entry.find('='));
            for (const OperatorSetting& setting : kOperatorSettings) {
                if (name == setting.tunable) {
                    add(entry);
                }
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
    __has_feature(memory_sanitizer) || __has_feature(hwaddress_sanitizer) ||                       \
    __has_feature(leak_sanitizer)
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
