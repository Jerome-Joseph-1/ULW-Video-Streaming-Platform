#include "os/system_clock.hpp"

#include "uring_reactor.hpp"

#include <linux/capability.h>
#include <sys/resource.h>
#include <sys/syscall.h>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <fstream>
#include <gtest/gtest.h>
#include <string>
#include <unistd.h>
#include <utility>

namespace {

using net::detail::is_initial_user_namespace;
using net::detail::locked_memory_is_uncharged;
using net::detail::LockedMemoryFacts;

constexpr std::size_t kSlots = 64;
// The kernel's and systemd's default limit, which a reactor's zero-copy burst alone fills.
constexpr rlim_t kDefaultLimit = rlim_t{8} << 20U;
constexpr std::uint32_t kIpcLock = 1U << static_cast<unsigned>(CAP_IPC_LOCK);
// As the kernel prints it: each field right-aligned in a column of ten.
constexpr const char* kInitialMap = "         0          0 4294967295\n";
// Root of a rootless container: uid 0 inside is the invoking user's uid outside.
constexpr const char* kContainerMap = "         0       1000          1\n";

LockedMemoryFacts facts(std::expected<rlim_t, int> limit, std::expected<std::uint32_t, int> caps,
                        std::string uid_map) {
    return LockedMemoryFacts{
        .memlock_limit = limit, .effective_caps = caps, .uid_map = std::move(uid_map)};
}

TEST(UringLockedMemory, TheInitialNamespacesIdentityMapIsRecognisedHoweverPadded) {
    EXPECT_TRUE(is_initial_user_namespace(kInitialMap));
    EXPECT_TRUE(is_initial_user_namespace("0 0 4294967295"));
    EXPECT_TRUE(is_initial_user_namespace("\t0\t0\t4294967295\n\n"));
}

TEST(UringLockedMemory, AnyOtherMapIsANestedNamespace) {
    EXPECT_FALSE(is_initial_user_namespace(kContainerMap));
    // A namespace mapping only part of the id range, even identically.
    EXPECT_FALSE(is_initial_user_namespace("0 0 65536\n"));
    EXPECT_FALSE(is_initial_user_namespace("1 1 4294967295\n"));
    // A second range: some id does not map to itself.
    EXPECT_FALSE(is_initial_user_namespace("0 0 4294967295\n0 100000 65536\n"));
}

TEST(UringLockedMemory, AMapThatCannotBeReadOrParsedIsNotTrusted) {
    EXPECT_FALSE(is_initial_user_namespace(""));
    EXPECT_FALSE(is_initial_user_namespace(" \n"));
    EXPECT_FALSE(is_initial_user_namespace("0 0"));
    EXPECT_FALSE(is_initial_user_namespace("0 0 4294967295x"));
    EXPECT_FALSE(is_initial_user_namespace("0x0 0 4294967295"));
    EXPECT_FALSE(is_initial_user_namespace("-0 0 4294967295"));
    EXPECT_FALSE(is_initial_user_namespace("0 0 99999999999999999999999"));
}

TEST(UringLockedMemory, AnUnlimitedLimitIsNeverCharged) {
    EXPECT_TRUE(locked_memory_is_uncharged(facts(RLIM_INFINITY, 0U, kContainerMap)));
    // However the capability check would go.
    EXPECT_TRUE(locked_memory_is_uncharged(facts(RLIM_INFINITY, std::unexpected(EPERM), "")));
}

TEST(UringLockedMemory, AFiniteLimitIsChargedWithoutCapIpcLock) {
    EXPECT_FALSE(locked_memory_is_uncharged(facts(kDefaultLimit, 0U, kInitialMap)));
    // Every capability but CAP_IPC_LOCK.
    EXPECT_FALSE(locked_memory_is_uncharged(facts(kDefaultLimit, ~kIpcLock, kInitialMap)));
}

TEST(UringLockedMemory, CapIpcLockExemptsOnlyInTheInitialUserNamespace) {
    EXPECT_TRUE(locked_memory_is_uncharged(facts(kDefaultLimit, kIpcLock, kInitialMap)));
    // A container's root reports the capability, which the charge ignores.
    EXPECT_FALSE(locked_memory_is_uncharged(facts(kDefaultLimit, kIpcLock, kContainerMap)));
    EXPECT_FALSE(locked_memory_is_uncharged(facts(kDefaultLimit, kIpcLock, "")));
}

TEST(UringLockedMemory, WhatCannotBeReadCountsAsCharged) {
    // capget refused (EPERM under a seccomp filter, say): no exemption is assumed.
    EXPECT_FALSE(
        locked_memory_is_uncharged(facts(kDefaultLimit, std::unexpected(EPERM), kInitialMap)));
    // getrlimit failed: only the capability can still exempt.
    EXPECT_FALSE(locked_memory_is_uncharged(facts(std::unexpected(EFAULT), 0U, kInitialMap)));
    EXPECT_TRUE(locked_memory_is_uncharged(facts(std::unexpected(EFAULT), kIpcLock, kInitialMap)));
    EXPECT_FALSE(locked_memory_is_uncharged(
        facts(std::unexpected(EFAULT), std::unexpected(EPERM), kInitialMap)));
}

TEST(UringLockedMemory, TheFactsReadAreTheProcesssOwn) {
    const LockedMemoryFacts read = net::detail::read_locked_memory_facts();

    rlimit limit{};
    ASSERT_EQ(::getrlimit(RLIMIT_MEMLOCK, &limit), 0);
    ASSERT_TRUE(read.memlock_limit.has_value()) << "errno " << read.memlock_limit.error();
    EXPECT_EQ(*read.memlock_limit, limit.rlim_cur);

    __user_cap_header_struct header{.version = _LINUX_CAPABILITY_VERSION_3, .pid = 0};
    std::array<__user_cap_data_struct, _LINUX_CAPABILITY_U32S_3> caps{};
    ASSERT_EQ(::syscall(SYS_capget, &header, caps.data()), 0);
    ASSERT_TRUE(read.effective_caps.has_value()) << "errno " << read.effective_caps.error();
    EXPECT_EQ(*read.effective_caps, caps[0].effective);

    std::ifstream map("/proc/self/uid_map");
    std::string expected;
    for (std::string line; std::getline(map, line);) {
        expected += line + '\n';
    }
    EXPECT_EQ(read.uid_map, expected);
    EXPECT_FALSE(read.uid_map.empty());
}

// The zero-copy probe's burst would charge the user's locked memory with up to 8 MiB, so a
// process the kernel charges never makes it, and its reactor sends by copy. Nothing is left in
// flight that could still hold the charge.
TEST(UringLockedMemory, AChargedReactorNeverProbesZeroCopy) {
    os::SystemClock clock;
    for (const LockedMemoryFacts& charged :
         {facts(kDefaultLimit, 0U, kInitialMap), facts(kDefaultLimit, kIpcLock, kContainerMap),
          facts(kDefaultLimit, std::unexpected(EPERM), kInitialMap)}) {
        auto reactor = net::detail::UringReactor::create(clock, kSlots, charged);
        ASSERT_TRUE(reactor) << "errno " << reactor.error();
        EXPECT_FALSE((*reactor)->zero_copy_supported());
        EXPECT_EQ((*reactor)->sends_in_flight(), 0U);
    }
}

} // namespace
