#include "ops/allocator.hpp"

#include <cstddef>
#include <cstdlib>
#include <gtest/gtest.h>
#include <malloc.h>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using Call = std::pair<int, int>;

// A process to tune: an environment, which allocator it has, and a mallopt that records what it
// is asked and refuses `refuse`.
struct FakeProcess {
    std::map<std::string, std::string, std::less<>> env;
    bool jemalloc = false;
    bool sanitizer = false;
    std::optional<int> refuse;
    std::vector<Call> calls;

    ops::AllocatorSeams seams() {
        return {.env = [this](std::string_view name) -> std::optional<std::string> {
                    const auto it = env.find(name);
                    return it == env.end() ? std::nullopt : std::optional(it->second);
                },
                .jemalloc = jemalloc,
                .sanitizer = sanitizer,
                .mallopt =
                    [this](int option, int value) {
                        calls.emplace_back(option, value);
                        return option == refuse ? 0 : 1;
                    }};
    }
};

const std::vector<Call> kTuned{
    {M_ARENA_MAX, 1}, {M_MMAP_THRESHOLD, 131'072}, {M_TRIM_THRESHOLD, 131'072}};

TEST(TuneAllocator, SetsOneArenaAndFixedThresholdsOnGlibc) {
    FakeProcess p;
    const auto report = ops::tune_allocator(ops::MallocTuning{}, p.seams());
    ASSERT_TRUE(report) << report.error();
    EXPECT_EQ(report->choice, ops::AllocatorChoice::Tuned);
    EXPECT_EQ(report->description, "glibc arena_max=1 mmap_threshold=131072 trim_threshold=131072");
    EXPECT_EQ(p.calls, kTuned);
}

TEST(TuneAllocator, AppliesAndReportsTheValuesItIsGiven) {
    FakeProcess p;
    const auto report = ops::tune_allocator(
        {.arena_max = 2, .mmap_threshold = 65'536, .trim_threshold = 262'144}, p.seams());
    ASSERT_TRUE(report) << report.error();
    EXPECT_EQ(report->description, "glibc arena_max=2 mmap_threshold=65536 trim_threshold=262144");
    EXPECT_EQ(p.calls,
              (std::vector<Call>{
                  {M_ARENA_MAX, 2}, {M_MMAP_THRESHOLD, 65'536}, {M_TRIM_THRESHOLD, 262'144}}));
}

TEST(TuneAllocator, ARefusedOptionIsAnErrorNamingItAndStopsThere) {
    FakeProcess p;
    p.refuse = M_MMAP_THRESHOLD;
    const auto report = ops::tune_allocator(ops::MallocTuning{}, p.seams());
    ASSERT_FALSE(report);
    EXPECT_EQ(report.error(), "mallopt(M_MMAP_THRESHOLD, 131072) refused by glibc's malloc");
    EXPECT_EQ(p.calls, (std::vector<Call>{{M_ARENA_MAX, 1}, {M_MMAP_THRESHOLD, 131'072}}));
}

TEST(TuneAllocator, LeavesMallocToAnOperatorsMallocVariable) {
    FakeProcess p;
    p.env = {{"MALLOC_ARENA_MAX", "2"}, {"MALLOC_TRIM_THRESHOLD_", "0"}};
    const auto report = ops::tune_allocator(ops::MallocTuning{}, p.seams());
    ASSERT_TRUE(report) << report.error();
    EXPECT_EQ(report->choice, ops::AllocatorChoice::Operator);
    EXPECT_EQ(report->description, "glibc MALLOC_ARENA_MAX=2 MALLOC_TRIM_THRESHOLD_=0");
    EXPECT_TRUE(p.calls.empty());
}

TEST(TuneAllocator, LeavesMallocToAnOperatorsMallocTunable) {
    FakeProcess p;
    p.env = {{"GLIBC_TUNABLES", "glibc.cpu.hwcaps=-AVX2:glibc.malloc.mmap_threshold=65536"}};
    const auto report = ops::tune_allocator(ops::MallocTuning{}, p.seams());
    ASSERT_TRUE(report) << report.error();
    EXPECT_EQ(report->choice, ops::AllocatorChoice::Operator);
    EXPECT_EQ(report->description, "glibc glibc.malloc.mmap_threshold=65536");
    EXPECT_TRUE(p.calls.empty());
}

TEST(TuneAllocator, TunablesOfOtherPartsOfGlibcAndEmptyVariablesAreNoMallocSetting) {
    FakeProcess p;
    p.env = {{"GLIBC_TUNABLES", "glibc.cpu.hwcaps=-AVX2:glibc.rtld.nns=2"},
             {"MALLOC_ARENA_MAX", ""}};
    const auto report = ops::tune_allocator(ops::MallocTuning{}, p.seams());
    ASSERT_TRUE(report) << report.error();
    EXPECT_EQ(report->choice, ops::AllocatorChoice::Tuned);
    EXPECT_EQ(p.calls, kTuned);
}

TEST(TuneAllocator, LeavesJemallocAlone) {
    FakeProcess p;
    p.jemalloc = true;
    p.env = {{"MALLOC_ARENA_MAX", "2"}};
    const auto report = ops::tune_allocator(ops::MallocTuning{}, p.seams());
    ASSERT_TRUE(report) << report.error();
    EXPECT_EQ(report->choice, ops::AllocatorChoice::Jemalloc);
    EXPECT_EQ(report->description, "jemalloc");
    EXPECT_TRUE(p.calls.empty());
}

TEST(TuneAllocator, LeavesASanitizersAllocatorAlone) {
    FakeProcess p;
    p.sanitizer = true;
    const auto report = ops::tune_allocator(ops::MallocTuning{}, p.seams());
    ASSERT_TRUE(report) << report.error();
    EXPECT_EQ(report->choice, ops::AllocatorChoice::Sanitizer);
    EXPECT_EQ(report->description, "sanitizer");
    EXPECT_TRUE(p.calls.empty());
}

TEST(TuneAllocator, TheProcessSeamsSeeTheBuildsSanitizerAndNoJemalloc) {
    const auto seams = ops::process_allocator([](std::string_view) { return std::nullopt; });
    // The unit tests link no jemalloc (ADR-0081).
    EXPECT_FALSE(seams.jemalloc);
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
    EXPECT_TRUE(seams.sanitizer);
#endif
}

// Kept where the compiler cannot see it unused, so the allocations below are not elided.
void* volatile g_held = nullptr;

std::size_t mapped_chunks() {
    return mallinfo2().hblks;
}

// The real thing, in this process: once tuned, freeing a large mapped buffer no longer raises
// glibc's mmap threshold, so a 192 KiB buffer after it is still a mapping of its own. Untuned,
// the free would raise the threshold to 1 MiB and the 192 KiB would come from the heap.
TEST(TuneAllocator, FixesGlibcsMmapThresholdInThisProcess) {
    const auto report = ops::tune_allocator(
        ops::MallocTuning{}, ops::process_allocator([](std::string_view name) {
            // The process's own environment, as the gateway reads it.
            // NOLINTNEXTLINE(concurrency-mt-unsafe): no other thread sets the environment
            const char* value = std::getenv(std::string(name).c_str());
            return value == nullptr ? std::nullopt : std::optional<std::string>(value);
        }));
    ASSERT_TRUE(report) << report.error();
    if (report->choice != ops::AllocatorChoice::Tuned) {
        GTEST_SKIP() << "malloc is not glibc's to tune here: " << report->description;
    }
    // NOLINTBEGIN(cppcoreguidelines-no-malloc,cppcoreguidelines-owning-memory): malloc itself
    // is under test
    g_held = std::malloc(std::size_t{1} << 20U);
    ASSERT_NE(g_held, nullptr);
    std::free(g_held);
    const std::size_t before = mapped_chunks();
    g_held = std::malloc(std::size_t{192} << 10U);
    ASSERT_NE(g_held, nullptr);
    EXPECT_EQ(mapped_chunks(), before + 1);
    std::free(g_held);
    // NOLINTEND(cppcoreguidelines-no-malloc,cppcoreguidelines-owning-memory)
    g_held = nullptr;
}

} // namespace
