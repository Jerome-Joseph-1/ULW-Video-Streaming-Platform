#include "core/version.hpp"
#include "infra/auth/local_verifier.hpp"
#include "infra/packagers/kubernetes_packagers.hpp"
#include "infra/packagers/process_packagers.hpp"
#include "infra/postgres/health_check.hpp"
#include "infra/postgres/live_streams.hpp"
#include "infra/postgres/upload_catalog.hpp"
#include "infra/s3util/credentials.hpp"
#include "infra/sfu/livekit/livekit_sfu.hpp"
#include "infra/storage/fs_store.hpp"
#include "infra/storage/s3_store.hpp"
#include "net/offload_pool.hpp"
#include "net/signals.hpp"
#include "net/socket.hpp"
#include "net/transport.hpp"
#include "os/limits.hpp"
#include "os/system_clock.hpp"
#include "os/system_random.hpp"
#include "os/unique_fd.hpp"

#include "config.hpp"
#include "gateway.hpp"
#include "health.hpp"
#include "key_fetcher.hpp"
#include "ops/async_log.hpp"
#include "ops/log.hpp"
#include "ops/notify.hpp"
#include "ops/process.hpp"
#include "ops/root.hpp"
#include "ops/settings.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <memory>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <system_error>
#include <unistd.h>
#include <vector>

namespace {

// 65536 descriptors: 448 client connections plus their backend sockets need a few thousand,
// and the reactor's descriptor-indexed slot table stays at a few megabytes.
constexpr std::size_t kMaxDescriptors = 65'536;
// Wakes the loop at least this often, for the drain check and the watchdog, whose interval
// is seconds.
constexpr core::Millis kLoopTick{1'000};
// What configuration errors exit with, so a supervisor can tell a deployment that will never
// start from one that crashed.
constexpr int kExitConfig = 2;
// 256 KiB holds about 850 request lines of ~300 bytes: at the gateway's busiest, 448 uploads
// each finishing an 8 MiB chunk no faster than every 0.67 s (100 Mbit/s), with playlist reads
// besides, well over a second of lines. A log reader stalled longer than that costs lines,
// counted, rather than the loop.
constexpr std::size_t kLogBuffer = std::size_t{256} * 1024;
// At exit, a log reader gets this long to take what is queued. The manager kills the process
// 45 s after SIGTERM (TimeoutStopSec, and the pod's grace period); the preStop hook's 5 s and
// the 30 s drain leave 10 s, of which the exit needs this at most.
constexpr core::Millis kLogFlushLimit{2'000};

std::optional<std::string> read_env(std::string_view name) {
    // Read once, before any thread exists, so nothing can race it with setenv.
    // NOLINTNEXTLINE(concurrency-mt-unsafe)
    const char* value = std::getenv(std::string(name).c_str());
    return value == nullptr ? std::nullopt : std::optional<std::string>(value);
}

std::string errno_text(int error) {
    return std::generic_category().message(error);
}

int fail(ops::Logger& log, std::string_view what, std::string_view why) {
    log.error("startup failed", {{"step", what}, {"error", why}});
    return EXIT_FAILURE;
}

int refuse(ops::Logger& log, std::string_view source, std::string_view reason) {
    log.error("configuration refused", {{"source", source}, {"reason", reason}});
    return kExitConfig;
}

// Started as root (by hand, or by a supervisor that stays root to raise a limit), the process
// becomes the configured user before it opens a socket or starts a thread; not root, there is
// nothing to give up. nullopt means carry on, anything else is the exit code.
std::optional<int> leave_root(const std::string& user, bool allow_root, ops::Logger& log) {
    const auto step = ops::leave_root(user, allow_root);
    if (!step) {
        return step.error().configuration ? refuse(log, step.error().source, step.error().reason)
                                          : fail(log, step.error().source, step.error().reason);
    }
    if (*step == ops::RootStep::StayedRoot) {
        log.warn("running as root, as ULW_ALLOW_ROOT=1 allows");
    } else if (*step == ops::RootStep::Dropped) {
        log.info("dropped root", {{"user", user}});
    }
    return std::nullopt;
}

// Owns everything the gateway borrows, in construction order, so that destruction runs in
// reverse: the gateway goes before the store and catalog its connections point at.
struct Services {
    explicit Services(ops::Logger& logger) : log(logger) {}

    ops::Logger& log;
    os::SystemClock clock;
    os::SystemRandom random;
    gateway::Health health;
    std::unique_ptr<net::IReactor> reactor;
    std::unique_ptr<net::ITransportFactory> transports;
    std::unique_ptr<net::OffloadPool> pool;
    std::unique_ptr<infra::curl::Multi> multi;
    // Key set fetches get a multi of their own: the store's is capped at max_upload_slots
    // connections, and slow uploads holding all of them would otherwise queue a key refresh
    // behind them.
    std::unique_ptr<infra::curl::Multi> key_multi;
    std::unique_ptr<infra::s3util::EnvCredentialProvider> credentials;
    std::unique_ptr<core::ports::IIngestStore> store;
    // The same object as `store`, seen as a reader.
    core::ports::IObjectReader* reader = nullptr;
    // The object store's count of failures that need an operator; none for the filesystem.
    std::function<std::uint64_t()> paging_errors;
    std::unique_ptr<infra::postgres::PgUploadCatalog> catalog;
    std::unique_ptr<gateway::KeySetFetcher> key_fetcher;
    std::unique_ptr<core::ports::IJwtVerifier> verifier;
    // The stream service and what it drives, when live publishing is configured (ADR-0091).
    // LiveKit and the Kubernetes API get a multi of their own, as key fetches do, so that no
    // upload holding the store's connections delays a ticket.
    std::unique_ptr<infra::curl::Multi> live_multi;
    std::unique_ptr<core::ports::ISfu> sfu;
    std::unique_ptr<core::ports::IPackagers> packagers;
    std::unique_ptr<infra::postgres::PgLiveStreams> live_store;
    std::unique_ptr<gateway::LiveStreams> live;
    // LiveKit's webhooks, on their own listener, when it is configured (ADR-0093).
    std::unique_ptr<gateway::PublisherWatch> watch;
    std::unique_ptr<gateway::WebhookServer> webhooks;
    std::unique_ptr<gateway::Gateway> gateway;
    std::unique_ptr<net::SignalWatcher> signals;
    std::unique_ptr<infra::postgres::PgHealthCheck> database_check;
    // Last, so its thread stops before anything it asks goes away.
    std::unique_ptr<gateway::HealthProbe> probe;

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
        // Its own writers, so a slow disk never queues the control calls behind it. Uploads
        // write one job at a time each; four threads keep four disks' worth of fsyncs apart.
        constexpr std::size_t kFsWriters = 4;
        auto writers = net::OffloadPool::create(*s.reactor, kFsWriters);
        if (!writers) {
            return std::unexpected("fs writer pool: " + errno_text(writers.error()));
        }
        auto fs = std::make_unique<infra::storage::FsStore>(
            infra::storage::FsStore::Deps{.clock = s.clock, .random = s.random},
            std::move(*writers), config.storage_location, config.chunk_size);
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
                                                  .bucket = config.bucket},
                                                 {.part_size = config.chunk_size});
    if (!store) {
        return std::unexpected("object store configuration refused");
    }
    infra::storage::S3Store* s3 = store->get();
    s.paging_errors = [s3] { return s3->paging_errors(); };
    s.reader = s3;
    s.store = std::move(*store);
    return {};
}

std::expected<void, std::string> make_verifier(const gateway::Config& config, Services& s) {
    infra::auth::ClaimRules rules{.issuer = config.jwt_issuer, .audience = config.jwt_audience};
    if (!config.dev_jwks_file.empty()) {
        auto local = infra::auth::Ed25519LocalVerifier::create(config.dev_jwks, std::move(rules));
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
    s.key_fetcher = std::make_unique<gateway::KeySetFetcher>(*s.key_multi, s.log);
    ops::Logger& log = s.log;
    s.verifier = std::make_unique<infra::auth::JwksVerifier>(
        *s.reactor, *s.key_fetcher,
        infra::auth::JwksConfig{
            .url = config.jwks_url,
            .claims = std::move(rules),
            .max_key_age = std::chrono::hours(config.jwks_max_stale_hours),
            .on_keys_expired = [&log](core::Millis age) noexcept {
                log.error("jwks keys expired",
                          {{"hours_without_refresh",
                            std::chrono::duration_cast<std::chrono::hours>(age).count()}});
            }});
    return {};
}

std::expected<void, std::string> make_live(const gateway::Config& config, Services& s) {
    const gateway::LiveConfig& live = config.live;
    if (!live.enabled) {
        return {};
    }
    auto multi = infra::curl::Multi::create(*s.reactor);
    if (!multi) {
        return std::unexpected("libcurl multi for the stream service failed to start");
    }
    s.live_multi = std::move(*multi);
    auto sfu = infra::sfu::livekit::make_sfu(
        *s.reactor, *s.live_multi, s.clock,
        infra::sfu::livekit::Config{.api_url = live.livekit_api_url,
                                    .client_url = live.livekit_client_url,
                                    .api_key = live.livekit_api_key,
                                    .api_secret = live.livekit_api_secret,
                                    .packager_srt = live.packager_srt});
    if (!sfu) {
        return std::unexpected(std::string(infra::sfu::livekit::to_string(sfu.error())));
    }
    s.sfu = std::move(*sfu);
    if (live.runtime == gateway::PackagerRuntime::Process) {
        auto packagers = infra::packagers::ProcessPackagers::create(
            *s.reactor, {.binary = live.packager_binary, .environment = live.packager_environment});
        if (!packagers) {
            return std::unexpected(std::move(packagers.error()));
        }
        s.packagers = std::move(*packagers);
    } else {
        auto packagers = infra::packagers::KubernetesPackagers::create(
            *s.reactor, *s.live_multi, *s.pool, s.clock,
            {.api_url = live.k8s_api_url,
             .namespace_name = live.k8s_namespace,
             .token_file = live.k8s_token_file,
             .ca_file = live.k8s_ca_file,
             .job_template = live.job_template,
             .image_tag = live.image_tag});
        if (!packagers) {
            return std::unexpected(std::move(packagers.error()));
        }
        s.packagers = std::move(*packagers);
    }
    auto store = infra::postgres::PgLiveStreams::create(
        *s.reactor, *s.pool, infra::postgres::LiveStreamsConfig{.conninfo = config.database_url});
    if (!store) {
        return std::unexpected(std::move(store.error()));
    }
    s.live_store = std::move(*store);
    s.live = std::make_unique<gateway::LiveStreams>(gateway::LiveDeps{.reactor = *s.reactor,
                                                                      .store = *s.live_store,
                                                                      .sfu = *s.sfu,
                                                                      .packagers = *s.packagers,
                                                                      .clock = s.clock,
                                                                      .random = s.random,
                                                                      .log = s.log},
                                                    live.settings);
    s.live->start_sweeping();
    if (live.webhook_port != 0) {
        s.watch = std::make_unique<gateway::PublisherWatch>(*s.reactor, *s.live, s.log, live.watch);
        s.webhooks = std::make_unique<gateway::WebhookServer>(
            *s.reactor, s.clock,
            gateway::WebhookKey{.id = live.livekit_api_key, .secret = live.livekit_api_secret},
            *s.watch, s.log, gateway::WebhookLimits{});
    }
    return {};
}

// A key nothing ever writes: NotFound proves the store answers, and costs one GET.
constexpr std::string_view kProbeKey = "health/probe";

gateway::ProbeChecks probe_checks(Services& s) {
    return {.database = [&s] { return s.database_check->oldest_queued_job(); },
            .store = [&s]() -> std::expected<void, std::string> {
                const auto key = core::StorageKey::parse(kProbeKey);
                if (!key) {
                    return std::unexpected("probe key refused");
                }
                const auto got = s.reader->fetch_small(*key, 1);
                if (got || got.error() == core::ports::StorageError::NotFound) {
                    return {};
                }
                return std::unexpected(std::string(core::ports::to_string(got.error())));
            },
            .store_paging_errors = s.paging_errors};
}

int serve(const gateway::Config& config, const os::NofileLimits& limits, os::UniqueFd listener,
          os::UniqueFd webhook_listener, ops::Logger& log,
          const std::optional<ops::Notifier>& notifier) {
    const auto info = core::build_info();
    Services s(log);
    auto choice = net::make_reactor_with_fallback(config.reactor, s.clock, limits.soft);
    if (!choice) {
        return fail(log, "reactor", errno_text(choice.error()));
    }
    s.reactor = std::move(choice->reactor);
    if (auto r = make_transports(config, s); !r) {
        return fail(log, "ULW_TRANSPORT=tls", r.error());
    }
    auto pool = net::OffloadPool::create(*s.reactor, config.offload_threads);
    if (!pool) {
        return fail(log, "offload pool", errno_text(pool.error()));
    }
    s.pool = std::move(*pool);
    // One store connection per admitted upload, so a part never queues behind slow ones
    // (ADR-0045); ADR-0027 already budgets a backend socket for each.
    auto multi = infra::curl::Multi::create(*s.reactor, config.limits.max_upload_slots);
    if (!multi) {
        return fail(log, "libcurl", "no threaded resolver; name lookups would block the loop");
    }
    s.multi = std::move(*multi);
    if (auto r = make_store(config, s); !r) {
        return fail(log, "storage", r.error());
    }
    auto catalog = infra::postgres::PgUploadCatalog::create(
        *s.reactor, *s.pool, infra::postgres::CatalogConfig{.conninfo = config.database_url});
    if (!catalog) {
        return fail(log, "ULW_DATABASE_URL", catalog.error());
    }
    s.catalog = std::move(*catalog);
    if (auto r = make_verifier(config, s); !r) {
        return fail(log, "auth", r.error());
    }
    if (auto r = make_live(config, s); !r) {
        return fail(log, "live", r.error());
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
                                                                 .random = s.random,
                                                                 .log = log,
                                                                 .health = s.health,
                                                                 .live_streams = s.live.get(),
                                                                 .webhooks = s.webhooks.get(),
                                                                 .publisher_watch = s.watch.get()},
                                                   config.limits);
    auto signals = net::SignalWatcher::create(*s.reactor, *s.gateway);
    if (!signals) {
        return fail(log, "signalfd", errno_text(signals.error()));
    }
    s.signals = std::move(*signals);
    if (auto r = s.reactor->listen(std::move(listener), *s.gateway); !r) {
        return fail(log, "register listener", errno_text(r.error()));
    }
    if (s.webhooks) {
        if (auto r = s.reactor->listen(std::move(webhook_listener), *s.webhooks); !r) {
            return fail(log, "register webhook listener", errno_text(r.error()));
        }
    }
    s.database_check = std::make_unique<infra::postgres::PgHealthCheck>(config.database_url);
    s.probe = std::make_unique<gateway::HealthProbe>(s.health, probe_checks(s), s.clock, log);
    s.probe->start(gateway::kProbeInterval);
    // Says which keys tokens are checked against, so a development key set left configured in
    // a real deployment shows on the first line of the log.
    log.info("listening",
             {{"version", info.version},
              {"git_sha", info.git_sha},
              {"port", config.port},
              {"webhook_port", config.live.webhook_port},
              {"transport", config.transport == gateway::Transport::Tls ? "tls" : "plain"},
              {"reactor", net::to_string(choice->kind)},
              {"io_uring_unavailable", choice->fell_back_from_io_uring.has_value()},
              {"nofile", limits.soft},
              {"keys", config.dev_jwks_file.empty() ? config.jwks_url
                                                    : "DEVELOPMENT " + config.dev_jwks_file}});

    // Started means serving /healthz; /readyz follows the probe.
    if (notifier) {
        notifier->ready();
    }
    const auto watchdog = notifier ? notifier->watchdog_interval() : std::nullopt;
    core::MonoTime next_ping = s.clock.now();
    bool told_stopping = false;
    while (!s.gateway->finished()) {
        s.reactor->run_once(kLoopTick);
        s.gateway->reap();
        if (s.webhooks) {
            s.webhooks->reap();
        }
        // Only a loop that turns pings: a wedged loop is what the watchdog is for.
        if (watchdog && s.clock.now() >= next_ping) {
            notifier->watchdog();
            next_ping = s.clock.now() + *watchdog;
        }
        if (s.gateway->draining() && !told_stopping) {
            s.probe->stop();
            if (notifier) {
                notifier->stopping();
            }
            told_stopping = true;
        }
    }
    log.info("drained");
    return EXIT_SUCCESS;
}

int run(std::span<const std::string_view> args) {
    const auto info = core::build_info();
    const os::SystemClock clock;
    ops::StdoutSink direct;
    ops::Logger boot(direct, clock, "gateway", ops::Level::Info);
    // First, before the configuration and its secrets are read: see ops::disable_core_dumps.
    if (auto r = ops::disable_core_dumps(); !r) {
        return fail(boot, "disable core dumps", errno_text(r.error()));
    }

    const auto cli = ops::parse_command_line(gateway::settings(), args);
    if (!cli) {
        return refuse(boot, cli.error().source, cli.error().reason);
    }
    if (cli->version) {
        std::println("gateway_server {} ({})", info.version, info.git_sha);
        return EXIT_SUCCESS;
    }
    const auto layers = ops::load_settings(gateway::settings(), *cli, read_env);
    if (!layers) {
        return refuse(boot, layers.error().source, layers.error().reason);
    }
    const auto config = gateway::load_config(layers->lookup());
    if (!config) {
        return refuse(boot, config.error().variable, config.error().reason);
    }
    boot.set_threshold(config->log_level);
    const std::string_view jemalloc = ops::jemalloc_version();
    boot.info("starting", {{"version", info.version},
                           {"git_sha", info.git_sha},
                           {"allocator", jemalloc.empty() ? "default" : "jemalloc"},
                           {"allocator_version", jemalloc}});
    gateway::log_effective(*config, *layers, boot);
    // Before any thread exists, so every thread inherits the mask.
    if (auto r = net::block_shutdown_signals(); !r) {
        return fail(boot, "block signals", errno_text(r.error()));
    }
    const auto limits = os::raise_nofile_limit(kMaxDescriptors);
    if (!limits) {
        return fail(boot, "raise RLIMIT_NOFILE", errno_text(limits.error()));
    }
    if (auto r = gateway::check_descriptor_budget(config->limits, limits->soft); !r) {
        return refuse(boot, r.error().variable, r.error().reason);
    }
    // Bound while still root, if started so: a port under 1024 needs the privilege the drop
    // gives up.
    os::UniqueFd listener;
    os::UniqueFd webhook_listener;
    if (!cli->check) {
        auto bound = net::listen_tcp({.port = config->port});
        if (!bound) {
            return fail(boot, "listen", errno_text(bound.error()));
        }
        listener = std::move(*bound);
        if (config->live.webhook_port != 0) {
            auto hooks = net::listen_tcp({.port = config->live.webhook_port});
            if (!hooks) {
                return fail(boot, "listen for webhooks", errno_text(hooks.error()));
            }
            webhook_listener = std::move(*hooks);
        }
    }
    // Before any thread exists: glibc then has no other thread to carry the change to.
    if (const auto code = leave_root(config->run_as_user, config->allow_root, boot)) {
        return *code;
    }
    if (cli->check) {
        boot.info("configuration valid");
        return EXIT_SUCCESS;
    }
    auto notifier = ops::Notifier::from_env(read_env, ::getpid());
    if (!notifier) {
        return refuse(boot, "NOTIFY_SOCKET", errno_text(notifier.error()));
    }

    auto sink = ops::AsyncLogSink::create(STDOUT_FILENO, kLogBuffer, kLogFlushLimit);
    if (!sink) {
        return fail(boot, "log sink", errno_text(sink.error()));
    }
    int code = EXIT_FAILURE;
    {
        ops::Logger log(**sink, clock, "gateway", config->log_level);
        code = serve(*config, *limits, std::move(listener), std::move(webhook_listener), log,
                     *notifier);
    }
    const std::uint64_t dropped = (*sink)->close();
    if (dropped > 0) {
        // Stdout's reader is the one that fell behind, and the metric that counted the loss is
        // gone with the listener, so the total goes where the manager still reads.
        ops::StdoutSink stderr_sink(STDERR_FILENO);
        ops::Logger last(stderr_sink, clock, "gateway", ops::Level::Warn);
        last.warn("log lines dropped", {{"count", dropped}});
    }
    return code;
}

} // namespace

// Formatting and allocation are all that can still throw; report it and exit.
int main(int argc, char** argv) {
    try {
        const std::span<char*> raw(argv, static_cast<std::size_t>(argc));
        std::vector<std::string_view> args;
        for (const char* a : raw.subspan(1)) {
            args.emplace_back(a);
        }
        return run(args);
    } catch (const std::exception& e) {
        static_cast<void>(std::fputs(e.what(), stderr));
        return EXIT_FAILURE;
    } catch (...) {
        return EXIT_FAILURE;
    }
}
