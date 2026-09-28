#include "core/version.hpp"

#include <cstdio>
#include <print>

int main() try {
    const auto info = core::build_info();
    std::println("gateway_server {} ({})", info.version, info.git_sha);
    return 0;
} catch (...) {
    static_cast<void>(std::fputs("gateway_server: failed to write to stdout\n", stderr));
    return 1;
}
