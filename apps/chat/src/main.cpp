#include "core/util/json.hpp"
#include "core/version.hpp"
#include "infra/auth/jwks_verifier.hpp"
#include "infra/auth/local_verifier.hpp"
#include "infra/curl/multi.hpp"
#include "infra/postgres/message_store.hpp"
#include "infra/postgres/room_store.hpp"
#include "net/offload_pool.hpp"
#include "net/signals.hpp"
#include "net/socket.hpp"
#include "os/limits.hpp"
#include "os/system_clock.hpp"
#include "os/system_random.hpp"
#include "rt/room_router.hpp"

#include "chat.hpp"
#include "config.hpp"
#include "key_fetcher.hpp"
#include "log.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <memory>
#include <optional>
#include <print>
#include <string>
#include <system_error>

namespace {

// Clients up to chat::Limits::max_connections, a few node-channel and database sockets, and
// headroom; the reactor's descriptor-indexed table stays small.
constexpr std::size_t kMaxDescriptors = 8'192;
// Wakes the loop at least this often; nothing depends on it but the drain check.
constexpr core::Millis kLoopTick{1'000};
// Only the database host's name is looked up off the loop, once per connection attempt.
constexpr std::size_t kOffloadThreads = 1;
// Section 8.14: bad configuration exits 2, anything else that stops startup exits 1.
constexpr int kBadConfig = 2;

std::optional<std::string> read_env(std::string_view name) {
    // Read once, before any thread exists, so nothing can race it with setenv.
    // NOLINTNEXTLINE(concurrency-mt-unsafe)
    const char* value = std::getenv(std::string(name).c_str());
    return value == nullptr ? std::nullopt : std::optional<std::string>(value);
}

std::optional<std::string> read_key_set(const std::string& path) {
    // A development key set holds one or two Ed25519 keys, a few hundred bytes.
    constexpr std::size_t kMaxKeySet = std::size_t{64} * 1024;
    std::ifstream in(path, std::ios::binary);
    std::string out;
    // One page per read; the whole file is at most sixteen of them.
    std::array<char, 4096> buf{};
    while (in) {
        in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        out.append(buf.data(), static_cast<std::size_t>(in.gcount()));
        if (out.size() > kMaxKeySet) {
            return std::nullopt;
        }
    }
    if (!in.eof()) {
        return std::nullopt;
    }
    return out;
}

std::string errno_text(int error) {
    return std::generic_category().message(error);
}

int fail(std::string_view what, std::string_view why, int code = EXIT_FAILURE) {
    std::println(stderr, "chat_server: {}: {}", what, why);
    return code;
}

// Owns everything the server borrows, in construction order.
struct Services {
    os::SystemClock clock;
    os::SystemRandom random;
    std::unique_ptr<net::IReactor> reactor;
    std::unique_ptr<net::OffloadPool> offload;
    std::unique_ptr<infra::postgres::PgRoomStore> store;
    std::unique_ptr<infra::postgres::PgMessageStore> messages;
    std::unique_ptr<infra::curl::Multi> key_multi;
    std::unique_ptr<chat::KeySetFetcher> key_fetcher;
    std::unique_ptr<core::ports::IJwtVerifier> verifier;
    std::unique_ptr<chat::RoomLog> room_log;
    std::unique_ptr<rt::RoomRouter> router;
    std::unique_ptr<chat::ChatServer> server;
    std::unique_ptr<net::SignalWatcher> signals;

    Services() = default;
    Services(const Services&) = delete;
    Services& operator=(const Services&) = delete;

    ~Services() {
        // A lookup running on the pool points into the stores; the room store's answers point
        // into the router, and the message store's into the chat service, which the server
        // holds; the server's sessions leave their rooms through the router.
        offload.reset();
        signals.reset();
        messages.reset();
        server.reset();
        store.reset();
    }
};

std::expected<void, std::string> make_verifier(const chat::Config& config, Services& s) {
    infra::auth::ClaimRules rules{.issuer = config.jwt_issuer, .audience = config.jwt_audience};
    if (!config.dev_jwks_file.empty()) {
        const auto jwks = read_key_set(config.dev_jwks_file);
        if (!jwks) {
            return std::unexpected("cannot read " + config.dev_jwks_file);
        }
        auto local = infra::auth::Ed25519LocalVerifier::create(*jwks, std::move(rules));
        if (!local) {
            return std::unexpected(std::string(infra::auth::to_string(local.error())));
        }
        s.verifier = std::make_unique<infra::auth::Ed25519LocalVerifier>(std::move(*local));
        return {};
    }
    auto key_multi = infra::curl::Multi::create(*s.reactor);
    if (!key_multi) {
        return std::unexpected("libcurl multi for key fetches failed to start");
    }
    s.key_multi = std::move(*key_multi);
    s.key_fetcher = std::make_unique<chat::KeySetFetcher>(*s.key_multi);
    s.verifier = std::make_unique<infra::auth::JwksVerifier>(
        *s.reactor, *s.key_fetcher,
        infra::auth::JwksConfig{.url = config.jwks_url, .claims = std::move(rules)});
    return {};
}

int run() {
    const auto info = core::build_info();
    auto config = chat::load_config(read_env);
    if (!config) {
        return fail(config.error().variable, config.error().reason, kBadConfig);
    }
    if (auto r = net::block_shutdown_signals(); !r) {
        return fail("block signals", errno_text(r.error()));
    }
    const auto limits = os::raise_nofile_limit(kMaxDescriptors);
    if (!limits) {
        return fail("raise RLIMIT_NOFILE", errno_text(limits.error()));
    }

    Services s;
    auto choice = net::make_reactor_with_fallback(config->reactor, s.clock, limits->soft);
    if (!choice) {
        return fail("reactor", errno_text(choice.error()));
    }
    s.reactor = std::move(choice->reactor);
    auto offload = net::OffloadPool::create(*s.reactor, kOffloadThreads);
    if (!offload) {
        return fail("offload pool", errno_text(offload.error()));
    }
    s.offload = std::move(*offload);
    auto store = infra::postgres::PgRoomStore::create(*s.reactor, *s.offload,
                                                      {.conninfo = config->database_url});
    if (!store) {
        // libpq's reason quotes the offending part of the string, which may be the password.
        return fail("ULW_DATABASE_URL", "not a connection string this server can use", kBadConfig);
    }
    s.store = std::move(*store);
    auto messages = infra::postgres::PgMessageStore::create(*s.reactor, *s.offload,
                                                            {.conninfo = config->database_url});
    if (!messages) {
        return fail("ULW_DATABASE_URL", "not a connection string this server can use", kBadConfig);
    }
    s.messages = std::move(*messages);
    if (auto r = make_verifier(*config, s); !r) {
        return fail("auth", r.error());
    }

    s.room_log = std::make_unique<chat::RoomLog>(config->node);
    s.router = std::make_unique<rt::RoomRouter>(*s.reactor, *s.store, s.clock, s.random,
                                                rt::RouterConfig{.self = config->node,
                                                                 .advertise = config->node_address,
                                                                 .secret = config->node_secret},
                                                *s.room_log);
    // Only the address other nodes dial, never every interface (ADR-0035).
    auto node_listener = net::listen_on(config->node_address);
    if (!node_listener) {
        return fail("listen on the node port", errno_text(node_listener.error()));
    }
    if (auto r = s.router->start(std::move(*node_listener)); !r) {
        return fail("register the node listener", errno_text(r.error()));
    }

    chat::Limits chat_limits;
    if (const std::optional<core::Millis> grace = config->presence_grace) {
        chat_limits.presence.grace = *grace;
    }
    s.server = std::make_unique<chat::ChatServer>(
        chat::Deps{.node = config->node,
                   .reactor = *s.reactor,
                   .router = *s.router,
                   .messages = *s.messages,
                   .verifier = *s.verifier,
                   .clock = s.clock},
        chat::Access{.cookie = config->auth_cookie, .allowed_origins = config->allowed_origins},
        chat_limits);
    auto signals = net::SignalWatcher::create(*s.reactor, *s.server);
    if (!signals) {
        return fail("signalfd", errno_text(signals.error()));
    }
    s.signals = std::move(*signals);
    auto listener = net::listen_tcp({.port = config->port});
    if (!listener) {
        return fail("listen", errno_text(listener.error()));
    }
    if (auto r = s.reactor->listen(std::move(*listener), *s.server); !r) {
        return fail("register listener", errno_text(r.error()));
    }
    // Says which keys tokens are checked against, so a development key set left configured in
    // a real deployment shows on the first line of the log.
    std::string keys;
    core::json::append_string(keys, config->dev_jwks_file.empty()
                                        ? config->jwks_url
                                        : "DEVELOPMENT " + config->dev_jwks_file);
    chat::log_event(
        R"("level":"info","msg":"listening","version":"{}","git":"{}","node":"{}","port":{},)"
        R"("node_address":"{}","reactor":"{}{}","keys":{})",
        info.version, info.git_sha, config->node.view(), config->port, config->node_address,
        net::to_string(choice->kind), choice->fell_back_from_io_uring ? " (fallback)" : "", keys);

    while (!s.server->finished()) {
        s.reactor->run_once(kLoopTick);
        s.server->reap();
    }
    chat::log_event(R"("level":"info","msg":"drained","node":"{}")", config->node.view());
    return EXIT_SUCCESS;
}

} // namespace

// Formatting and allocation are all that can still throw outside the loop; report it and exit.
int main() {
    try {
        return run();
    } catch (const std::exception& e) {
        static_cast<void>(std::fputs(e.what(), stderr));
        return EXIT_FAILURE;
    } catch (...) {
        return EXIT_FAILURE;
    }
}
