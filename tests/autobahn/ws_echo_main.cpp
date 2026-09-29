#include "core/util/parse.hpp"
#include "net/reactor_factory.hpp"
#include "net/signals.hpp"
#include "net/socket.hpp"
#include "os/system_clock.hpp"

#include "ws_echo_server.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <optional>
#include <print>
#include <span>
#include <string_view>

namespace {

struct Args {
    std::uint16_t port = 9001;
    net::ReactorKind reactor = net::ReactorKind::IoUring;
    // Autobahn's case 9 echoes messages of up to 16 MiB; its limit cases go no further.
    std::uint64_t max_message_bytes = std::uint64_t{16} * 1024 * 1024;
};

template <class T> bool parse_number(std::string_view text, T& out) {
    const std::optional<T> v = core::parse_integer<T>(text);
    if (v) {
        out = *v;
    }
    return v.has_value();
}

bool parse(std::span<char* const> argv, Args& args) {
    if (const char* env = std::getenv("ULW_REACTOR")) {
        const auto kind = net::parse_reactor_kind(env);
        if (!kind) {
            return false;
        }
        args.reactor = *kind;
    }
    if (argv.size() % 2 != 1) {
        return false;
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
        } else if (flag == "--max-message-bytes") {
            ok = parse_number(value, args.max_message_bytes);
        }
        if (!ok) {
            return false;
        }
    }
    return true;
}

int run(int argc, char** argv) {
    Args args;
    if (!parse(std::span{argv, static_cast<std::size_t>(argc)}, args)) {
        std::println(stderr, "usage: ulw_ws_echo_server [--port N] [--reactor io_uring|epoll] "
                             "[--max-message-bytes N]");
        return 2;
    }
    if (auto r = net::block_shutdown_signals(); !r) {
        std::println(stderr, "block signals: {}", std::strerror(r.error()));
        return 1;
    }
    // A few hundred connections at most: Autobahn runs its cases one at a time.
    constexpr std::size_t kMaxFds = 4'096;
    os::SystemClock clock;
    auto choice = net::make_reactor_with_fallback(args.reactor, clock, kMaxFds);
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
    ulw::test::WsEchoServer server(*choice->reactor, {.max_message_bytes = args.max_message_bytes});
    if (auto r = choice->reactor->listen(std::move(*listener), server); !r) {
        std::println(stderr, "register listener: {}", std::strerror(r.error()));
        return 1;
    }
    auto signals = net::SignalWatcher::create(*choice->reactor, server);
    if (!signals) {
        std::println(stderr, "signalfd: {}", std::strerror(signals.error()));
        return 1;
    }
    std::println("listening port={} reactor={}{}", port.value_or(0), net::to_string(choice->kind),
                 choice->fell_back_from_io_uring ? " (io_uring unavailable)" : "");
    static_cast<void>(std::fflush(stdout));

    while (!server.finished()) {
        choice->reactor->run_once(core::Millis{1'000});
        server.reap();
    }
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
