// ulw_reaper: one pass of the upload reaper, meant for a CronJob. Aborts uploads past their
// expires_at that are still active, releases their storage sessions, and sweeps sessions with
// no upload behind them. Prints the pass's counters in Prometheus text format on stdout and
// exits non-zero if any part of it failed.
#include "core/version.hpp"
#include "infra/postgres/upload_reaper.hpp"
#include "infra/s3util/credentials.hpp"
#include "infra/storage/fs_store.hpp"
#include "infra/storage/s3_store.hpp"
#include "net/offload_pool.hpp"
#include "net/reactor_factory.hpp"
#include "os/system_clock.hpp"
#include "os/system_random.hpp"

#include "config.hpp"
#include "ops/root.hpp"
#include "reaper.hpp"

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <expected>
#include <memory>
#include <optional>
#include <print>
#include <string>
#include <system_error>

namespace {

// One reactor descriptor table entry per socket the store may open; a pass talks to one
// server at a time.
constexpr std::size_t kMaxDescriptors = 1024;
constexpr int kFailed = 1;

std::optional<std::string> read_env(std::string_view name) {
    // Read once, before any thread exists, so nothing can race it with setenv.
    // NOLINTNEXTLINE(concurrency-mt-unsafe)
    const char* value = std::getenv(std::string(name).c_str());
    return value == nullptr ? std::nullopt : std::optional<std::string>(value);
}

// What a configuration that can never run exits with, as for the gateway and the worker.
constexpr int kBadConfig = 2;

int fail(std::string_view what, std::string_view why) {
    std::println(stderr, "ulw_reaper: {}: {}", what, why);
    return kFailed;
}

// The store and what it borrows, in construction order.
struct Services {
    os::SystemClock clock;
    os::SystemRandom random;
    std::unique_ptr<net::IReactor> reactor;
    std::unique_ptr<infra::curl::Multi> multi;
    std::unique_ptr<infra::s3util::EnvCredentialProvider> credentials;
    // The store as the ports the reaper uses see it.
    core::ports::IIngestStore* ingest = nullptr;
    core::ports::IObjectAdmin* admin = nullptr;
    std::unique_ptr<infra::storage::S3Store> s3;
    std::unique_ptr<infra::storage::FsStore> fs;
};

std::expected<void, std::string> make_store(const reaper::Config& config, Services& s) {
    if (config.storage == reaper::StorageBackend::Filesystem) {
        // Nothing is written, so one writer thread that stays idle.
        auto writers = net::OffloadPool::create(*s.reactor, 1);
        if (!writers) {
            return std::unexpected("fs writer pool: " +
                                   std::generic_category().message(writers.error()));
        }
        // The chunk size only matters to uploads that open sessions, which a reaper does not.
        constexpr std::uint64_t kChunk = std::uint64_t{8} << 20U;
        s.fs = std::make_unique<infra::storage::FsStore>(
            infra::storage::FsStore::Deps{.clock = s.clock, .random = s.random},
            std::move(*writers), config.storage_location, kChunk);
        s.ingest = s.fs.get();
        s.admin = s.fs.get();
        return {};
    }
    auto profile = config.storage == reaper::StorageBackend::R2
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
    auto multi = infra::curl::Multi::create(*s.reactor);
    if (!multi) {
        return std::unexpected("libcurl multi failed to start");
    }
    s.multi = std::move(*multi);
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
    s.s3 = std::move(*store);
    s.ingest = s.s3.get();
    s.admin = s.s3.get();
    return {};
}

int run() {
    const auto info = core::build_info();
    const auto config = reaper::load_config(read_env);
    if (!config) {
        return fail(config.error().variable, config.error().reason);
    }
    // Before any thread exists: glibc then has no other thread to carry the change to.
    if (const auto step = ops::leave_root(config->run_as_user, config->allow_root); !step) {
        const int code = fail(step.error().source, step.error().reason);
        return step.error().configuration ? kBadConfig : code;
    }
    Services services;
    auto reactor = net::make_reactor(net::ReactorKind::Epoll, services.clock, kMaxDescriptors);
    if (!reactor) {
        return fail("reactor", std::generic_category().message(reactor.error()));
    }
    services.reactor = std::move(*reactor);
    if (const auto made = make_store(*config, services); !made) {
        return fail("storage", made.error());
    }
    infra::postgres::PgUploadReaper uploads(config->database_url);
    const reaper::Report report =
        reaper::run_once(uploads, *services.ingest, *services.admin, services.clock,
                         {.batch = 100, .orphan_after = config->orphan_after});
    std::println(stderr,
                 "ulw_reaper: {} ({}) expired {} uploads, {} not released, aborted {} orphaned "
                 "sessions",
                 info.version, info.git_sha, report.uploads_expired, report.uploads_release_failed,
                 report.parts_orphaned);
    for (const std::string& problem : report.problems) {
        std::println(stderr, "ulw_reaper: {}", problem);
    }
    std::print("{}", reaper::metrics_text(report));
    return report.problems.empty() ? EXIT_SUCCESS : kFailed;
}

} // namespace

// Formatting and allocation are all that can still throw; report it and exit.
int main() {
    try {
        return run();
    } catch (const std::exception& e) {
        static_cast<void>(std::fputs(e.what(), stderr));
        return kFailed;
    } catch (...) {
        return kFailed;
    }
}
