// The gateway's memory with many uploads in flight at once, over plain or TLS (M9 acceptance).
//
//   ulw_gateway_load --transport tls|plain --uploads N --reactor io_uring|epoll
//
// The gateway runs in this process and its clients in a forked child, so the RSS this process
// reports is the gateway's alone. It is sampled three times:
//   baseline:  the gateway listening, nothing connected;
//   connected: every client has finished its handshake and created an upload;
//   stalled:   every client is part way into an 8 MiB chunk the store takes none of, so every
//              connection holds all the gateway ever buffers for one upload: the parser's
//              staging, the transport's parked ciphertext and the TLS session.
// Prints one key=value line and exits non-zero if a stalled upload cost more than the 348 KB a
// connection is budgeted (docs/adr/0007).
#include "core/util/json.hpp"
#include "infra/catalog/memory_catalog.hpp"
#include "infra/storage/fake_store.hpp"
#include "net/offload_pool.hpp"
#include "net/reactor_factory.hpp"
#include "net/socket.hpp"
#include "net/transport.hpp"
#include "os/system_clock.hpp"
#include "os/system_random.hpp"

#include "config.hpp"
#include "gateway.hpp"
#include "support/fake_verifier.hpp"
#include "support/http_client.hpp"
#include "support/tls_pki.hpp"

#include <sys/wait.h>

#include <atomic>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

// Every term of the per-connection budget in ADR-0007, kernel buffers and backend socket
// included, which this process does not even hold: an upper bound, not a target.
constexpr std::uint64_t kBudgetPerConnection = std::uint64_t{348} * 1000;
constexpr std::uint64_t kChunk = std::uint64_t{8} * 1024 * 1024;
// No more bytes taken for this long means every client is blocked on a full socket.
constexpr auto kSettle = std::chrono::seconds(2);
// Short of the gateway's 30 s body timeout, which would start answering the stalled uploads.
constexpr auto kPhaseLimit = std::chrono::seconds(25);

struct Args {
    gateway::Transport transport = gateway::Transport::Tls;
    std::size_t uploads = 500;
    net::ReactorKind reactor = net::ReactorKind::IoUring;
};

bool parse(int argc, char** argv, Args& args) {
    const auto all = std::span(argv, static_cast<std::size_t>(argc));
    const std::vector<std::string_view> words(all.begin() + 1, all.end());
    if (words.size() % 2 != 0) {
        return false;
    }
    for (std::size_t i = 0; i < words.size(); i += 2) {
        const std::string_view flag = words[i];
        const std::string_view value = words[i + 1];
        if (flag == "--transport" && (value == "tls" || value == "plain")) {
            args.transport = value == "tls" ? gateway::Transport::Tls : gateway::Transport::Plain;
        } else if (flag == "--uploads") {
            const char* last = std::to_address(value.end());
            const auto [p, ec] =
                std::from_chars(std::to_address(value.begin()), last, args.uploads);
            if (ec != std::errc{} || p != last || args.uploads == 0 || args.uploads > 512) {
                return false;
            }
        } else if (flag == "--reactor" && net::parse_reactor_kind(value)) {
            args.reactor = *net::parse_reactor_kind(value);
        } else {
            return false;
        }
    }
    return true;
}

std::uint64_t status_kb(std::string_view field) {
    std::ifstream in("/proc/self/status");
    std::string line;
    while (std::getline(in, line)) {
        if (line.starts_with(field)) {
            std::uint64_t kb = 0;
            const std::string_view rest = std::string_view(line).substr(field.size());
            const std::string_view digits = rest.substr(rest.find_first_of("0123456789"));
            std::from_chars(std::to_address(digits.begin()), std::to_address(digits.end()), kb);
            return kb;
        }
    }
    return 0;
}

// One client: creates an upload, waits for the go, then sends one chunk until the gateway stops
// reading. Returns only when the socket gives up, which the parent's SIGKILL pre-empts.
void client(ulw::test::Endpoint endpoint, std::size_t index, std::atomic<std::size_t>& created,
            const std::atomic<bool>& go) {
    ulw::test::HttpClient c(endpoint);
    const std::string token = "user.load" + std::to_string(index);
    const std::string body = R"({"filename":"load.mp4","size_bytes":)" + std::to_string(kChunk) +
                             R"(,"content_type":"video/mp4"})";
    const auto r = c.request("POST", "/api/v1/uploads", token, std::as_bytes(std::span(body)));
    if (!r || r->status != 201) {
        return;
    }
    const auto doc = core::json::parse(r->body);
    if (!doc) {
        return;
    }
    const std::string upload(*doc->find("upload_id")->as_string());
    ++created;
    go.wait(false);
    const std::string head =
        "PATCH /api/v1/uploads/" + upload + " HTTP/1.1\r\nHost: load\r\nAuthorization: Bearer " +
        token + "\r\nUpload-Offset: 0\r\nContent-Length: " + std::to_string(kChunk) + "\r\n\r\n";
    if (!c.send_raw(head)) {
        return;
    }
    const std::vector<std::byte> piece(std::size_t{64} * 1024, std::byte{0x5a});
    for (std::uint64_t sent = 0; sent < kChunk; sent += piece.size()) {
        if (!c.send_raw(piece)) {
            return;
        }
    }
}

[[noreturn]] void run_clients(const Args& args, std::uint16_t port, int go_fd) {
    const auto ctx = args.transport == gateway::Transport::Tls
                         ? ulw::test::TestPki::shared().client_context()
                         : ulw::test::SslCtxPtr{};
    std::atomic<std::size_t> created{0};
    std::atomic<bool> go{false};
    std::vector<std::jthread> threads;
    threads.reserve(args.uploads);
    for (std::size_t i = 0; i < args.uploads; ++i) {
        threads.emplace_back(client, ulw::test::Endpoint{.port = port, .tls = ctx.get()}, i,
                             std::ref(created), std::cref(go));
    }
    char byte = 0;
    if (::read(go_fd, &byte, 1) == 1) {
        go = true;
        go.notify_all();
    }
    for (auto& t : threads) {
        t.join();
    }
    // The CA directory belongs to the parent; skip every destructor, including the PKI's.
    std::_Exit(EXIT_SUCCESS);
}

struct Server {
    os::SystemClock clock;
    os::SystemRandom random;
    std::unique_ptr<net::IReactor> reactor;
    std::unique_ptr<net::ITransportFactory> transports;
    std::unique_ptr<net::OffloadPool> pool;
    std::unique_ptr<infra::storage::FakeStore> store;
    std::unique_ptr<infra::catalog::MemoryCatalog> catalog;
    ulw::test::FakeVerifier verifier;
    std::unique_ptr<gateway::Gateway> gateway;

    Server() = default;
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;
    ~Server() {
        pool.reset();
        gateway.reset();
    }

    template <class Pred> bool run_until(Pred pred) {
        const auto deadline = Clock::now() + kPhaseLimit;
        while (!pred()) {
            if (Clock::now() > deadline) {
                return false;
            }
            reactor->run_once(core::Millis{50});
            gateway->reap();
        }
        return true;
    }
};

int run(const Args& args) {
    // Made before the fork, so the clients trust the CA whose certificate the server serves.
    const net::TlsFiles files = ulw::test::TestPki::shared().server();
    auto listener = net::listen_tcp({.port = 0, .loopback_only = true});
    if (!listener) {
        std::println(stderr, "gateway_load: listen failed");
        return EXIT_FAILURE;
    }
    const std::uint16_t port = *net::local_port(listener->get());
    std::array<int, 2> go{-1, -1};
    if (::pipe(go.data()) != 0) {
        return EXIT_FAILURE;
    }
    const pid_t child = ::fork();
    if (child == 0) {
        listener->reset();
        ::close(go[1]);
        run_clients(args, port, go[0]);
    }
    ::close(go[0]);

    Server s;
    s.reactor = std::move(*net::make_reactor(args.reactor, s.clock, 4096));
    if (args.transport == gateway::Transport::Tls) {
        s.transports = std::move(*net::make_tls_transports(*s.reactor, files));
    } else {
        s.transports = net::make_plain_transports(*s.reactor);
    }
    s.pool = std::move(*net::OffloadPool::create(*s.reactor, 4));
    s.store = std::make_unique<infra::storage::FakeStore>(*s.reactor, s.clock, kChunk);
    s.catalog = std::make_unique<infra::catalog::MemoryCatalog>(*s.reactor);
    s.gateway = std::make_unique<gateway::Gateway>(gateway::Deps{.reactor = *s.reactor,
                                                                 .transports = *s.transports,
                                                                 .pool = *s.pool,
                                                                 .store = *s.store,
                                                                 .catalog = *s.catalog,
                                                                 .verifier = s.verifier,
                                                                 .clock = s.clock,
                                                                 .random = s.random},
                                                   gateway::Limits{});
    if (!s.reactor->listen(std::move(*listener), *s.gateway)) {
        return EXIT_FAILURE;
    }
    const std::uint64_t baseline = status_kb("VmRSS:");

    const auto n = args.uploads;
    const bool connected = s.run_until([&] {
        return s.gateway->counters().requests >= n && s.gateway->connections() == n &&
               s.transports->handshakes_in_flight() == 0;
    });
    const std::uint64_t connected_kb = status_kb("VmRSS:");

    s.store->set_plan({.accept_zero = true});
    const char byte = 1;
    const bool released = ::write(go[1], &byte, 1) == 1;
    std::uint64_t last = 0;
    auto quiet_since = Clock::now();
    const bool stalled = released && s.run_until([&] {
        const std::uint64_t now = s.gateway->counters().bytes_ingested;
        if (now != last) {
            last = now;
            quiet_since = Clock::now();
        }
        return s.gateway->upload_slots_in_use() == n && Clock::now() - quiet_since > kSettle;
    });
    const std::uint64_t stalled_kb = status_kb("VmRSS:");
    const std::uint64_t peak_kb = status_kb("VmHWM:");
    const std::uint64_t slots = s.gateway->upload_slots_in_use();
    const std::uint64_t ingested = s.gateway->counters().bytes_ingested;

    ::kill(child, SIGKILL);
    ::waitpid(child, nullptr, 0);

    const auto per_conn = [&](std::uint64_t kb) { return (kb - baseline) * 1024 / n; };
    const std::uint64_t stalled_per_conn = per_conn(std::max(stalled_kb, baseline));
    std::println("transport={} reactor={} uploads={} baseline_kb={} connected_kb={} "
                 "stalled_kb={} peak_kb={} per_connection_connected_b={} "
                 "per_connection_stalled_b={} budget_b={} slots={} body_bytes_held={}",
                 args.transport == gateway::Transport::Tls ? "tls" : "plain",
                 net::to_string(args.reactor), n, baseline, connected_kb, stalled_kb, peak_kb,
                 per_conn(std::max(connected_kb, baseline)), stalled_per_conn, kBudgetPerConnection,
                 slots, ingested);
    if (!connected || !stalled) {
        std::println(stderr, "gateway_load: the {} phase did not complete",
                     connected ? "stalled" : "connected");
        return EXIT_FAILURE;
    }
    return stalled_per_conn <= kBudgetPerConnection ? EXIT_SUCCESS : EXIT_FAILURE;
}

} // namespace

int main(int argc, char** argv) {
    try {
        Args args;
        if (!parse(argc, argv, args)) {
            std::println(stderr, "usage: ulw_gateway_load [--transport tls|plain] "
                                 "[--uploads 1-512] [--reactor io_uring|epoll]");
            return 2;
        }
        return run(args);
    } catch (const std::exception& e) {
        static_cast<void>(std::fputs(e.what(), stderr));
        return EXIT_FAILURE;
    } catch (...) {
        return EXIT_FAILURE;
    }
}
