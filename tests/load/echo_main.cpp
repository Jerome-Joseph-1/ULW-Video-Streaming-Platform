#include "net/reactor_factory.hpp"
#include "net/signals.hpp"
#include "net/socket.hpp"
#include "os/limits.hpp"
#include "os/system_clock.hpp"

#include "echo_server.hpp"

#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <print>
#include <span>
#include <string_view>

namespace {

struct Args {
    std::uint16_t port = 7000;
    net::ReactorKind reactor = net::ReactorKind::IoUring;
    std::uint64_t idle_ms = 10'000;
    std::uint64_t max_connections = 10'000;
};

template <class T> bool parse_number(std::string_view text, T& out) {
    const char* const first = std::to_address(text.begin());
    const char* const last = std::to_address(text.end());
    const auto [ptr, ec] = std::from_chars(first, last, out);
    return ec == std::errc{} && ptr == last;
}

bool parse(std::span<char*> argv, Args& args) {
    if (const char* env = std::getenv("ULW_REACTOR")) {
        const auto kind = net::parse_reactor_kind(env);
        if (!kind) {
            return false;
        }
        args.reactor = *kind;
    }
    for (std::size_t i = 1; i + 1 < argv.size(); i += 2) {
        const std::string_view flag = argv[i];
        const std::string_view value = argv[i + 1];
        bool ok = false;
        if (flag == "--port") {
            ok = parse_number(value, args.port);
        } else if (flag == "--reactor") {
            const auto kind = net::parse_reactor_kind(value);
            ok = kind.has_value();
            if (ok) {
                args.reactor = *kind;
            }
        } else if (flag == "--idle-ms") {
            ok = parse_number(value, args.idle_ms);
        } else if (flag == "--max-connections") {
            ok = parse_number(value, args.max_connections);
        }
        if (!ok) {
            return false;
        }
    }
    return argv.size() % 2 == 1;
}

int run(int argc, char** argv) {
    Args args;
    if (!parse(std::span(argv, static_cast<std::size_t>(argc)), args)) {
        std::println(stderr, "usage: ulw_echo_server [--port N] [--reactor io_uring|epoll] "
                             "[--idle-ms N] [--max-connections N]");
        return 2;
    }
    if (auto r = net::block_shutdown_signals(); !r) {
        std::println(stderr, "block signals: {}", std::strerror(r.error()));
        return 1;
    }
    // 65536 descriptors: the per-connection slot table stays at a few megabytes.
    const auto limits = os::raise_nofile_limit(65'536);
    if (!limits) {
        std::println(stderr, "raise RLIMIT_NOFILE: {}", std::strerror(limits.error()));
        return 1;
    }
    os::SystemClock clock;
    auto choice = net::make_reactor_with_fallback(args.reactor, clock, limits->soft);
    if (!choice) {
        std::println(stderr, "reactor setup: {}", std::strerror(choice.error()));
        return 1;
    }
    auto listener = net::listen_tcp({.port = args.port});
    if (!listener) {
        std::println(stderr, "listen on {}: {}", args.port, std::strerror(listener.error()));
        return 1;
    }
    const auto port = net::local_port(listener->get());
    ulw::test::EchoServer server(*choice->reactor, {.idle_timeout = core::Millis{args.idle_ms},
                                                    .max_connections = args.max_connections});
    if (auto r = choice->reactor->listen(std::move(*listener), server); !r) {
        std::println(stderr, "register listener: {}", std::strerror(r.error()));
        return 1;
    }
    auto signals = net::SignalWatcher::create(*choice->reactor, server);
    if (!signals) {
        std::println(stderr, "signalfd: {}", std::strerror(signals.error()));
        return 1;
    }
    std::println("listening port={} reactor={} nofile={}{}", port.value_or(0),
                 net::to_string(choice->kind), limits->soft,
                 choice->fell_back_from_io_uring ? " (io_uring unavailable)" : "");
    static_cast<void>(std::fflush(stdout));

    while (!server.finished()) {
        choice->reactor->run_once(core::Millis{1'000});
        server.reap();
    }
    std::println("drained rejected={}", server.rejected());
    return 0;
}

} // namespace

// Formatting and allocation are all that can still throw; report it and exit.
int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception& e) {
        static_cast<void>(std::fputs(e.what(), stderr));
        return 1;
    } catch (...) {
        return 1;
    }
}
