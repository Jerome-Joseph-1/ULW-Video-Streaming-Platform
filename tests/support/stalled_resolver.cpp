// Built as a shared library so that the dynamic linker finds its getaddrinfo() ahead of libc's
// for every object in the process, libcurl's resolver thread included. That makes a slow DNS
// server reproducible without one, and without waiting on a clock.
#include "stalled_resolver.hpp"

#include <bit>
#include <condition_variable>
#include <cstdint>
#include <dlfcn.h>
#include <mutex>
#include <netdb.h>
#include <string_view>

namespace {

using GetAddrInfo = int (*)(const char*, const char*, const addrinfo*, addrinfo**);

struct Stall {
    std::mutex mutex;
    std::condition_variable released;
    std::size_t blocked = 0;
    // Bumped by each release; a lookup waits until it moves past the value it started with.
    std::uint64_t generation = 0;
};

Stall& stall() {
    static Stall s;
    return s;
}

bool stalls(std::string_view name) {
    constexpr std::string_view kDomain = ulw::test::kStalledDomain;
    return name == kDomain || (name.size() > kDomain.size() && name.ends_with(kDomain) &&
                               name[name.size() - kDomain.size() - 1] == '.');
}

} // namespace

// glibc's declaration names the parameters with reserved identifiers, which cannot be copied.
// NOLINTNEXTLINE(readability-inconsistent-declaration-parameter-name)
extern "C" int getaddrinfo(const char* node, const char* service, const addrinfo* hints,
                           addrinfo** res) {
    if (node != nullptr && stalls(node)) {
        Stall& s = stall();
        std::unique_lock lock(s.mutex);
        const std::uint64_t mine = s.generation;
        ++s.blocked;
        s.released.wait(lock, [&] { return s.generation != mine; });
        --s.blocked;
        return EAI_NONAME;
    }
    static const auto real = std::bit_cast<GetAddrInfo>(::dlsym(RTLD_NEXT, "getaddrinfo"));
    return real(node, service, hints, res);
}

namespace ulw::test {

std::size_t stalled_lookups() {
    Stall& s = stall();
    const std::scoped_lock lock(s.mutex);
    return s.blocked;
}

void release_stalled_lookups() {
    Stall& s = stall();
    {
        const std::scoped_lock lock(s.mutex);
        ++s.generation;
    }
    s.released.notify_all();
}

} // namespace ulw::test
