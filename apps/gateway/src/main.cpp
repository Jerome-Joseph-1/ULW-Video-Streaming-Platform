#include "core/version.hpp"
#include "infra/auth/local_verifier.hpp"
#include "infra/postgres/upload_catalog.hpp"
#include "infra/s3util/credentials.hpp"
#include "infra/storage/fs_store.hpp"
#include "infra/storage/s3_store.hpp"
#include "net/offload_pool.hpp"
#include "net/signals.hpp"
#include "net/socket.hpp"
#include "net/transport.hpp"
#include "os/limits.hpp"
#include "os/system_clock.hpp"
#include "os/system_random.hpp"

#include "config.hpp"
#include "gateway.hpp"
#include "key_fetcher.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <memory>
#include <print>
#include <string>
#include <system_error>

namespace {

// 65536 descriptors: 448 client connections plus their backend sockets need a few thousand,
// and the reactor's descriptor-indexed slot table stays at a few megabytes.
constexpr std::size_t kMaxDescriptors = 65'536;
// Wakes the loop at least this often; nothing depends on it but the drain check.
constexpr core::Millis kLoopTick{1'000};

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

int fail(std::string_view what, std::string_view why) {
    std::println(stderr, "gateway_server: {}: {}", what, why);
    return EXIT_FAILURE;
}

// Owns everything the gateway borrows, in construction order, so that destruction runs in
// reverse: the gateway goes before the store and catalog its connections point at.
struct Services {
    os::SystemClock clock;
    os::SystemRandom random;
    std::unique_ptr<net::IReactor> reactor;
    std::unique_ptr<net::ITransportFactory> transports;
    std::unique_ptr<net::OffloadPool> pool;
    std::unique_ptr<infra::curl::Multi> multi;
    // Key set fetches get a multi of their own: the store's is capped at 64 connections, and
    // slow uploads holding all of them would otherwise queue a key refresh behind them.
    std::unique_ptr<infra::curl::Multi> key_multi;
    std::unique_ptr<infra::s3util::EnvCredentialProvider> credentials;
    std::unique_ptr<core::ports::IIngestStore> store;
    // The same object as `store`, seen as a reader.
    core::ports::IObjectReader* reader = nullptr;
    std::unique_ptr<infra::postgres::PgUploadCatalog> catalog;
    std::unique_ptr<gateway::KeySetFetcher> key_fetcher;
    std::unique_ptr<core::ports::IJwtVerifier> verifier;
    std::unique_ptr<gateway::Gateway> gateway;
    std::unique_ptr<net::SignalWatcher> signals;

    Services() = default;
    Services(const Services&) = delete;
    Services& operator=(const Services&) = delete;

    ~Services() {
        // A job still running on the pool points at a connection the gateway owns; the pool
        // must stop before the gateway, the catalog or the store can go.
        pool.reset();
    }
};

// The certificate and key are read here, so a deployment with unreadable or mismatched files
// fails at startup rather than on its first client.
std::expected<void, std::string> make_transports(const gateway::Config& config, Services& s) {
    if (config.transport == gateway::Transport::Plain) {
        s.transports = net::make_plain_transports(*s.reactor);
        return {};
    }
    auto tls =
        net::make_tls_transports(*s.reactor, {.certificate_chain = config.tls_certificate_chain,
                                              .private_key = config.tls_private_key});
    if (!tls) {
        return std::unexpected(std::move(tls.error()));
    }
    s.transports = std::move(*tls);
    return {};
}

std::expected<void, std::string> make_store(const gateway::Config& config, Services& s) {
    using gateway::StorageBackend;
    if (config.storage == StorageBackend::Filesystem) {
        // 8 MiB chunks, matching the object-store part size so clients see one chunk size.
        constexpr std::uint64_t kFsChunk = std::uint64_t{8} << 20U;
        // Its own writers, so a slow disk never queues the control calls behind it. Uploads
        // write one job at a time each; four threads keep four disks' worth of fsyncs apart.
        constexpr std::size_t kFsWriters = 4;
        auto writers = net::OffloadPool::create(*s.reactor, kFsWriters);
        if (!writers) {
            return std::unexpected("fs writer pool: " + errno_text(writers.error()));
        }
        auto fs = std::make_unique<infra::storage::FsStore>(
            infra::storage::FsStore::Deps{.clock = s.clock, .random = s.random},
            std::move(*writers), config.storage_location, kFsChunk);
        s.reader = fs.get();
        s.store = std::move(fs);
        return {};
    }
    auto profile = config.storage == StorageBackend::R2
                       ? infra::s3util::S3Profile::r2(config.storage_location)
                       : infra::s3util::S3Profile::minio(config.storage_location);
    if (!profile) {
        return std::unexpected("object store location is malformed");
    }
    auto credentials =
        infra::s3util::EnvCredentialProvider::load({.access_key_id = "ULW_S3_ACCESS_KEY_ID",
                                                    .secret_access_key = "ULW_S3_SECRET_ACCESS_KEY",
                                                    .session_token = std::nullopt});
    if (!credentials) {
        return std::unexpected(credentials.error().variable + " is unset or malformed");
    }
    s.credentials = std::make_unique<infra::s3util::EnvCredentialProvider>(std::move(*credentials));
    auto store = infra::storage::S3Store::create({.reactor = *s.reactor,
                                                  .multi = *s.multi,
                                                  .credentials = *s.credentials,
                                                  .clock = s.clock,
                                                  .random = s.random,
                                                  .profile = std::move(*profile),
                                                  .bucket = config.bucket});
    if (!store) {
        return std::unexpected("object store configuration refused");
    }
    s.reader = store->get();
    s.store = std::move(*store);
    return {};
}

std::expected<void, std::string> make_verifier(const gateway::Config& config, Services& s) {
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
    s.key_fetcher = std::make_unique<gateway::KeySetFetcher>(*s.key_multi);
    s.verifier = std::make_unique<infra::auth::JwksVerifier>(
        *s.reactor, *s.key_fetcher,
        infra::auth::JwksConfig{.url = config.jwks_url, .claims = std::move(rules)});
    return {};
}

int run() {
    const auto info = core::build_info();
    auto config = gateway::load_config(read_env);
    if (!config) {
        return fail(config.error().variable, config.error().reason);
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
    if (auto r = make_transports(*config, s); !r) {
        return fail("ULW_TRANSPORT=tls", r.error());
    }
    auto pool = net::OffloadPool::create(*s.reactor, config->offload_threads);
    if (!pool) {
        return fail("offload pool", errno_text(pool.error()));
    }
    s.pool = std::move(*pool);
    // One store connection per admitted upload, so a part never queues behind slow ones
    // (ADR-0039); ADR-0027 already budgets a backend socket for each.
    auto multi = infra::curl::Multi::create(*s.reactor, config->limits.max_upload_slots);
    if (!multi) {
        return fail("libcurl", "no threaded resolver; name lookups would block the loop");
    }
    s.multi = std::move(*multi);
    if (auto r = make_store(*config, s); !r) {
        return fail("storage", r.error());
    }
    auto catalog = infra::postgres::PgUploadCatalog::create(
        *s.reactor, *s.pool, infra::postgres::CatalogConfig{.conninfo = config->database_url});
    if (!catalog) {
        return fail("ULW_DATABASE_URL", catalog.error());
    }
    s.catalog = std::move(*catalog);
    if (auto r = make_verifier(*config, s); !r) {
        return fail("auth", r.error());
    }

    s.gateway = std::make_unique<gateway::Gateway>(gateway::Deps{.reactor = *s.reactor,
                                                                 .transports = *s.transports,
                                                                 .pool = *s.pool,
                                                                 .store = *s.store,
                                                                 .reader = *s.reader,
                                                                 .catalog = *s.catalog,
                                                                 .views = *s.catalog,
                                                                 .verifier = *s.verifier,
                                                                 .clock = s.clock,
                                                                 .random = s.random},
                                                   config->limits);
    auto signals = net::SignalWatcher::create(*s.reactor, *s.gateway);
    if (!signals) {
        return fail("signalfd", errno_text(signals.error()));
    }
    s.signals = std::move(*signals);
    auto listener = net::listen_tcp({.port = config->port});
    if (!listener) {
        return fail("listen", errno_text(listener.error()));
    }
    if (auto r = s.reactor->listen(std::move(*listener), *s.gateway); !r) {
        return fail("register listener", errno_text(r.error()));
    }
    // Says which keys tokens are checked against, so a development key set left configured in
    // a real deployment shows on the first line of the log.
    std::println(
        "gateway_server {} ({}) port={} transport={} reactor={}{} keys={}", info.version,
        info.git_sha, config->port, config->transport == gateway::Transport::Tls ? "tls" : "plain",
        net::to_string(choice->kind),
        choice->fell_back_from_io_uring ? " (io_uring unavailable)" : "",
        config->dev_jwks_file.empty() ? config->jwks_url : "DEVELOPMENT " + config->dev_jwks_file);
    static_cast<void>(std::fflush(stdout));

    while (!s.gateway->finished()) {
        s.reactor->run_once(kLoopTick);
        s.gateway->reap();
    }
    std::println("gateway_server drained");
    return EXIT_SUCCESS;
}

} // namespace

// Formatting and allocation are all that can still throw; report it and exit.
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
