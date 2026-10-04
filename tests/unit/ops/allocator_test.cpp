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

TEST(TuneAllocator, MallocSettingsItDoesNotMakeAreLeftToGlibcWhicheverWayTheyAreSpelt) {
    FakeProcess p;
    p.env = {{"GLIBC_TUNABLES",
              "glibc.cpu.hwcaps=-AVX2:glibc.malloc.perturb=165:glibc.malloc.check=3:"
              "glibc.malloc.arena_test=8:glibc.malloc.arena_max_typo=4"},
             {"MALLOC_PERTURB_", "165"},
             {"MALLOC_CHECK_", "3"},
             {"MALLOC_TOP_PAD_", "0"},
             {"MALLOC_ARENA_TEST", "8"}};
    const auto report = ops::tune_allocator(ops::MallocTuning{}, p.seams());
    ASSERT_TRUE(report) << report.error();
    EXPECT_EQ(report->choice, ops::AllocatorChoice::Tuned);
    EXPECT_EQ(p.calls, kTuned);
}

// glibc reads an empty MALLOC_ARENA_MAX as 0, below its minimum, and ignores it.
TEST(TuneAllocator, AnEmptyArenaMaxIsNoSetting) {
    FakeProcess p;
    p.env = {{"MALLOC_ARENA_MAX", ""}};
    const auto report = ops::tune_allocator(ops::MallocTuning{}, p.seams());
    ASSERT_TRUE(report) << report.error();
    EXPECT_EQ(report->choice, ops::AllocatorChoice::Tuned);
    EXPECT_EQ(p.calls, kTuned);
}

// glibc reads an empty MALLOC_MMAP_THRESHOLD_ or MALLOC_TRIM_THRESHOLD_ as 0, and applies it.
TEST(TuneAllocator, AnEmptyThresholdVariableIsTheOperatorsZero) {
    for (const char* name : {"MALLOC_MMAP_THRESHOLD_", "MALLOC_TRIM_THRESHOLD_"}) {
        FakeProcess p;
        p.env = {{name, ""}};
        const auto report = ops::tune_allocator(ops::MallocTuning{}, p.seams());
        ASSERT_TRUE(report) << report.error();
        EXPECT_EQ(report->choice, ops::AllocatorChoice::Operator) << name;
        EXPECT_EQ(report->description, std::string("glibc ") + name + "=");
        EXPECT_TRUE(p.calls.empty()) << name;
    }
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

std::optional<std::string> process_env(std::string_view name) {
    // NOLINTNEXTLINE(concurrency-mt-unsafe): no other thread sets the environment
    const char* value = std::getenv(std::string(name).c_str());
    return value == nullptr ? std::nullopt : std::optional<std::string>(value);
}

// 0 when the mmap threshold held: a 192 KiB buffer allocated after a 1 MiB mapped one was freed
// is still a mapping of its own. Untuned, that free raises glibc's threshold to 1 MiB and the
// 192 KiB comes from the heap.
int threshold_held() {
    // NOLINTBEGIN(cppcoreguidelines-no-malloc,cppcoreguidelines-owning-memory): malloc itself
    // is under test
    g_held = std::malloc(std::size_t{1} << 20U);
    std::free(g_held);
    const std::size_t before = mallinfo2().hblks;
    g_held = std::malloc(std::size_t{192} << 10U);
    const std::size_t after = mallinfo2().hblks;
    std::free(g_held);
    // NOLINTEND(cppcoreguidelines-no-malloc,cppcoreguidelines-owning-memory)
    g_held = nullptr;
    return after == before + 1 ? 0 : 1;
}

// The real thing, in a child process, so the tuning does not reach the other tests.
TEST(TuneAllocator, FixesGlibcsMmapThresholdInAProcess) {
    // What the real process would do, asked with a mallopt that changes nothing.
    auto probe = ops::process_allocator(process_env);
    probe.mallopt = [](int, int) { return 1; };
    const auto expected = ops::tune_allocator(ops::MallocTuning{}, probe);
    ASSERT_TRUE(expected) << expected.error();
    if (expected->choice != ops::AllocatorChoice::Tuned) {
        GTEST_SKIP() << "malloc is not glibc's to tune here: " << expected->description;
    }
    EXPECT_EXIT(
        {
            const auto report =
                ops::tune_allocator(ops::MallocTuning{}, ops::process_allocator(process_env));
            std::_Exit(report && report->choice == ops::AllocatorChoice::Tuned ? threshold_held()
                                                                               : 2);
        },
        ::testing::ExitedWithCode(0), "");
}

} // namespace
