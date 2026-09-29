#include "infra/postgres/migrator.hpp"

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <print>
#include <span>
#include <string>
#include <string_view>

namespace {

constexpr int kFailed = 1;
constexpr int kBadUsage = 2;

constexpr std::string_view kUsage = "usage: ulw_migrate [--status]\n"
                                    "Applies pending schema migrations to the database named by "
                                    "ULW_DATABASE_URL.\n";

int print_status(infra::postgres::Migrator& migrator) {
    auto rows = migrator.status(infra::postgres::bundled_migrations());
    if (!rows) {
        std::println(stderr, "ulw_migrate: {}", rows.error().message);
        return kFailed;
    }
    for (const auto& row : *rows) {
        if (!row.applied_at) {
            std::println("{:04} {:<24} pending", row.version, row.name);
            continue;
        }
        std::println("{:04} {:<24} {} {}", row.version, row.name,
                     row.unknown ? "applied, unknown to this build" : "applied", *row.applied_at);
    }
    return EXIT_SUCCESS;
}

int apply(infra::postgres::Migrator& migrator) {
    const auto known = infra::postgres::bundled_migrations();
    auto applied = migrator.apply(known);
    if (!applied) {
        std::println(stderr, "ulw_migrate: {}", applied.error().message);
        return kFailed;
    }
    if (applied->empty()) {
        std::println("schema is up to date");
    }
    for (const int version : *applied) {
        for (const auto& m : known) {
            if (m.version == version) {
                std::println("applied {:04}_{}", m.version, m.name);
            }
        }
    }
    return EXIT_SUCCESS;
}

int run(std::span<char*> args) {
    bool status = false;
    if (args.size() == 2 && std::string_view{args[1]} == "--status") {
        status = true;
    } else if (args.size() != 1) {
        std::print(stderr, "{}", kUsage);
        return kBadUsage;
    }
    // From the environment, never argv: arguments are visible to every user through /proc.
    // Read before any thread exists, so nothing can race it with setenv.
    // NOLINTNEXTLINE(concurrency-mt-unsafe)
    const char* url = std::getenv("ULW_DATABASE_URL");
    if (url == nullptr || *url == '\0') {
        std::println(stderr, "ulw_migrate: ULW_DATABASE_URL is not set");
        return kBadUsage;
    }
    auto migrator = infra::postgres::Migrator::connect(url);
    if (!migrator) {
        std::println(stderr, "ulw_migrate: {}", migrator.error().message);
        return kFailed;
    }
    return status ? print_status(*migrator) : apply(*migrator);
}

} // namespace

int main(int argc, char** argv) try {
    return run(std::span(argv, static_cast<std::size_t>(argc)));
} catch (const std::exception& e) {
    static_cast<void>(std::fprintf(stderr, "ulw_migrate: %s\n", e.what()));
    return kFailed;
}
