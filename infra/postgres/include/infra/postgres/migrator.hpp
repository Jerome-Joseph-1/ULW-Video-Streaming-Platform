#pragma once

#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace infra::postgres {

struct Migration {
    int version = 0;
    std::string_view name;
    // NUL-terminated; may hold several statements.
    const char* sql = nullptr;
};

// migrations/NNNN_name.sql, compiled in by the build and ordered by version, so a binary always
// carries exactly the schema history it was built against.
[[nodiscard]] std::span<const Migration> bundled_migrations() noexcept;

struct MigrationError {
    std::string message;
};

struct MigrationStatus {
    int version = 0;
    std::string name;
    // Postgres' text form of the time it was applied; nullopt while pending.
    std::optional<std::string> applied_at;
    // Applied by a newer build: the database is ahead of this binary.
    bool unknown = false;
};

// Applies migrations forward only, each in a transaction of its own. Servers never migrate on
// start; a deploy runs this once before rolling them out, and schema changes that must coexist
// with running code are split into expand and contract steps.
class Migrator {
public:
    // Blocks.
    [[nodiscard]] static std::expected<Migrator, MigrationError>
    connect(const std::string& conninfo);

    ~Migrator();
    Migrator(Migrator&&) noexcept;
    Migrator& operator=(Migrator&&) noexcept;
    Migrator(const Migrator&) = delete;
    Migrator& operator=(const Migrator&) = delete;

    // Every known and every applied version, ascending.
    [[nodiscard]] std::expected<std::vector<MigrationStatus>, MigrationError>
    status(std::span<const Migration> known);

    // Applies what is pending and returns the versions it applied. Fails without touching the
    // schema when another migrator is running, or when a pending version is older than one
    // already applied (history was rewritten).
    [[nodiscard]] std::expected<std::vector<int>, MigrationError>
    apply(std::span<const Migration> known);

private:
    class Impl;
    explicit Migrator(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_;
};

} // namespace infra::postgres
