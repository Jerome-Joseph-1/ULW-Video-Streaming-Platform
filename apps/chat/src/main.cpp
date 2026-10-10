#include "core/util/json.hpp"
#include "core/version.hpp"
#include "infra/auth/jwks_verifier.hpp"
#include "infra/auth/local_verifier.hpp"
#include "infra/curl/multi.hpp"
#include "infra/postgres/e2ee_directory.hpp"
#include "infra/postgres/message_store.hpp"
#include "infra/postgres/room_store.hpp"
#include "infra/sfu/livekit/livekit_sfu.hpp"
#include "net/offload_pool.hpp"
#include "net/signals.hpp"
#include "net/socket.hpp"
#include "os/limits.hpp"
#include "os/system_clock.hpp"
#include "os/system_random.hpp"
#include "os/unique_fd.hpp"
#include "rt/room_router.hpp"

#include "chat.hpp"
#include "config.hpp"
#include "key_fetcher.hpp"
#include "log.hpp"
#include "ops/process.hpp"
#include "ops/root.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <memory>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

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
    // The key directory (ADR-0102); its answers point into the server's directory service.
    std::unique_ptr<infra::postgres::PgE2eeDirectory> directory;
    std::unique_ptr<infra::curl::Multi> key_multi;
    std::unique_ptr<chat::KeySetFetcher> key_fetcher;
    std::unique_ptr<core::ports::IJwtVerifier> verifier;
    std::unique_ptr<chat::RoomLog> room_log;
    std::unique_ptr<rt::RoomRouter> router;
    // Calls (ADR-0050), when configured: LiveKit's server API on its own libcurl multi. The
    // server's call handler holds media rooms of the SFU and is destroyed first.
    std::unique_ptr<infra::curl::Multi> sfu_multi;
    std::unique_ptr<core::ports::ISfu> sfu;
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
        directory.reset();
        server.reset();
        store.reset();
    }
};

// Nothing when calls are not configured; LiveKit's adapter checks the values.
std::expected<void, std::string> make_sfu(const chat::Config& config, Services& s) {
    if (!config.calls) {
        return {};
    }
    auto multi = infra::curl::Multi::create(*s.reactor);
    if (!multi) {
        return std::unexpected("libcurl multi for LiveKit failed to start");
    }
    s.sfu_multi = std::move(*multi);
    const chat::CallsConfig& calls = *config.calls;
    auto sfu = infra::sfu::livekit::make_sfu(*s.reactor, *s.sfu_multi, s.clock,
                                             {.api_url = calls.api_url,
                                              .client_url = calls.client_url,
                                              .api_key = calls.api_key,
                                              .api_secret = calls.api_secret,
                                              .packager_srt = {}});
    if (!sfu) {
        return std::unexpected(std::string(infra::sfu::livekit::to_string(sfu.error())));
    }
    s.sfu = std::move(*sfu);
    return {};
}

std::expected<void, std::string> make_verifier(const chat::Config& config, Services& s) {
    infra::auth::ClaimRules rules{.issuer = config.jwt_issuer,
                                  .audience = config.jwt_audience,
                                  .subject_claim = config.jwt_subject_claim};
    rules.service_claim = config.service_claim;
    rules.service_value = config.service_value;
    rules.service_client_id = config.service_client_id;
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
        infra::auth::JwksConfig{
            .url = config.jwks_url,
            .claims = std::move(rules),
            .max_key_age = std::chrono::hours(config.jwks_max_stale_hours),
            .on_keys_expired = [](core::Millis age) noexcept {
                chat::log_event(
                    R"("level":"error","msg":"jwks keys expired","hours_without_refresh":{})",
                    std::chrono::duration_cast<std::chrono::hours>(age).count());
            }});
    return {};
}

// Where tickets send clients, or that calls are off, as a JSON string; never the key or the
// secret.
std::string calls_text(const chat::Config& config) {
    std::string out;
    core::json::append_string(out, config.calls ? config.calls->client_url : "off");
    return out;
}

// The key directory (ADR-0102) on the database the stores use. Two sessions: a claim's fetches
// queue on them, and claims come once per invitation. False when the URL cannot be used.
bool make_directory(const chat::Config& config, Services& s) {
    auto directory = infra::postgres::PgE2eeDirectory::create(
        *s.reactor, *s.offload, {.conninfo = config.database_url, .connections = 2});
    if (!directory) {
        return false;
    }
    s.directory = std::move(*directory);
    return true;
}

// The key directory, the token verifier and, when calls are configured, the SFU; the exit code
// when any cannot start.
std::optional<int> make_clients(const chat::Config& config, Services& s) {
    if (!make_directory(config, s)) {
        // libpq's reason quotes the offending part of the string, which may be the password.
        return fail("ULW_DATABASE_URL", "not a connection string this server can use", kBadConfig);
    }
    if (auto r = make_verifier(config, s); !r) {
        return fail("auth", r.error());
    }
    if (auto r = make_sfu(config, s); !r) {
        return fail("LIVEKIT_API_URL, LIVEKIT_CLIENT_URL, LIVEKIT_API_KEY, LIVEKIT_API_SECRET",
                    r.error(), kBadConfig);
    }
    return std::nullopt;
}

// The server's limits: its own, with what the configuration overrides.
chat::Limits limits_of(const chat::Config& config) {
    chat::Limits chat_limits;
    if (const std::optional<core::Millis> grace = config.presence_grace) {
        chat_limits.presence.grace = *grace;
    }
    if (const std::optional<core::Millis> ring = config.ring_timeout) {
        chat_limits.calls.ring.ring_timeout = *ring;
    }
    if (const std::optional<std::uint16_t> group = config.group_participants) {
        chat_limits.calls.group_participants = *group;
    }
    chat_limits.service.self_service = config.self_service;
    const chat::ClientLimits& per_client = config.client_limits;
    chat_limits.max_connections_per_ip =
        per_client.max_connections_per_ip.value_or(chat_limits.max_connections_per_ip);
    // Four /64s' worth, whatever the per-address cap was set to, and never past the node's own
    // cap, the most the variable may be set to (ADR-0076).
    constexpr std::size_t kSlash64sPerBlock = 4;
    chat_limits.max_connections_per_ip_block =
        per_client.max_connections_per_ip_block.value_or(std::min(
            kSlash64sPerBlock * chat_limits.max_connections_per_ip, chat_limits.max_connections));
    chat_limits.new_connections_per_ip_per_second =
        per_client.new_connections_per_ip_per_second.value_or(
            chat_limits.new_connections_per_ip_per_second);
    chat_limits.max_sessions_per_user =
        per_client.max_sessions_per_user.value_or(chat_limits.max_sessions_per_user);
    chat_limits.trusted_proxies = per_client.trusted_proxies;
    chat_limits.trusted_proxy_hops = per_client.trusted_proxy_hops;
    return chat_limits;
}

// The operator's backend's API, on a port of its own (ADR-0096), when configured; bound while
// still root, as the client port is.
std::expected<std::optional<os::UniqueFd>, int> listen_for_service(const chat::Config& config) {
    const std::optional<chat::ServiceApiConfig>& api = config.service_api;
    if (!api) {
        return std::nullopt;
    }
    auto bound = net::listen_tcp({.port = api->port});
    if (!bound) {
        return std::unexpected(bound.error());
    }
    return std::optional<os::UniqueFd>(std::move(*bound));
}

// The clients' listener and, when configured, the service API's, registered with the reactor;
// what failed names itself.
std::expected<void, std::pair<std::string_view, int>>
serve(Services& s, os::UniqueFd listener, std::optional<os::UniqueFd> service_listener) {
    if (auto r = s.reactor->listen(std::move(listener), *s.server); !r) {
        return std::unexpected(std::pair{std::string_view{"register listener"}, r.error()});
    }
    if (!service_listener) {
        return {};
    }
    if (auto r = s.reactor->listen(std::move(*service_listener), s.server->service_api()); !r) {
        return std::unexpected(
            std::pair{std::string_view{"register the service listener"}, r.error()});
    }
    return {};
}

int run() {
    const auto info = core::build_info();
    // First, before the configuration and its secrets are read: see ops::disable_core_dumps.
    if (auto r = ops::disable_core_dumps(); !r) {
        return fail("disable core dumps", errno_text(r.error()));
    }
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
    // Both bound while still root, if started so: a port under 1024 needs the privilege the
    // drop gives up. Only the address other nodes dial, never every interface (ADR-0035).
    auto node_listener = net::listen_on(config->node_address);
    if (!node_listener) {
        return fail("listen on the node port", errno_text(node_listener.error()));
    }
    auto listener = net::listen_tcp({.port = config->port});
    if (!listener) {
        return fail("listen", errno_text(listener.error()));
    }
    auto service_listener = listen_for_service(*config);
    if (!service_listener) {
        return fail("listen on the service port", errno_text(service_listener.error()));
    }
    // Before any thread exists: glibc then has no other thread to carry the change to.
    const auto step = ops::leave_root(config->run_as_user, config->allow_root);
    if (!step) {
        return fail(step.error().source, step.error().reason,
                    step.error().configuration ? kBadConfig : EXIT_FAILURE);
    }
    if (*step == ops::RootStep::StayedRoot) {
        chat::log_event(R"("level":"warn","msg":"running as root, as ULW_ALLOW_ROOT=1 allows")");
    } else if (*step == ops::RootStep::Dropped) {
        chat::log_event(R"("level":"info","msg":"dropped root","user":"{}")", config->run_as_user);
    }
    for (const unsigned prefix : chat::wide_trusted_proxies(config->client_limits)) {
        chat::log_event(R"("level":"warn","msg":"a trusted proxy block this wide lets many peers )"
                        R"(name any client","prefix_length":{})",
                        prefix);
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
    if (const std::optional<int> code = make_clients(*config, s)) {
        return *code;
    }

    s.room_log = std::make_unique<chat::RoomLog>(config->node);
    s.router = std::make_unique<rt::RoomRouter>(*s.reactor, *s.store, s.clock, s.random,
                                                rt::RouterConfig{.self = config->node,
                                                                 .advertise = config->node_address,
                                                                 .secret = config->node_secret},
                                                *s.room_log);
    if (auto r = s.router->start(std::move(*node_listener)); !r) {
        return fail("register the node listener", errno_text(r.error()));
    }

    const chat::Limits chat_limits = limits_of(*config);
    s.server = std::make_unique<chat::ChatServer>(
        chat::Deps{.node = config->node,
                   .reactor = *s.reactor,
                   .router = *s.router,
                   .messages = *s.messages,
                   .verifier = *s.verifier,
                   .clock = s.clock,
                   .random = s.random,
                   .sfu = s.sfu.get(),
                   .registry = s.directory.get(),
                   .key_packages = s.directory.get()},
        chat::Access{.cookie = config->auth_cookie, .allowed_origins = config->allowed_origins},
        chat_limits);
    auto signals = net::SignalWatcher::create(*s.reactor, *s.server);
    if (!signals) {
        return fail("signalfd", errno_text(signals.error()));
    }
    s.signals = std::move(*signals);
    if (auto r = serve(s, std::move(*listener), std::move(*service_listener)); !r) {
        return fail(r.error().first, errno_text(r.error().second));
    }
    // Says which keys tokens are checked against, so a development key set left configured in
    // a real deployment shows on the first line of the log.
    std::string keys;
    core::json::append_string(keys, config->dev_jwks_file.empty()
                                        ? config->jwks_url
                                        : "DEVELOPMENT " + config->dev_jwks_file);
    const std::string_view jemalloc = ops::jemalloc_version();
    const std::string calls = calls_text(*config);
    chat::log_event(
        R"("level":"info","msg":"listening","version":"{}","git":"{}","node":"{}","port":{},)"
        R"("node_address":"{}","reactor":"{}{}","keys":{},"allocator":"{}{}","calls":{},)"
        R"("self_service":{},"service_port":{})",
        info.version, info.git_sha, config->node.view(), config->port, config->node_address,
        net::to_string(choice->kind), choice->fell_back_from_io_uring ? " (fallback)" : "", keys,
        jemalloc.empty() ? "default" : "jemalloc ", jemalloc, calls, config->self_service,
        config->service_api.transform([](const chat::ServiceApiConfig& api) { return api.port; })
            .value_or(std::uint16_t{0}));

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
