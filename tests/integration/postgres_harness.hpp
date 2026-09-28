#pragma once

#include "sync_connection.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ulw::test {

// ULW_TEST_DATABASE_URL, or the server deploy/local/compose.yaml runs.
[[nodiscard]] std::string admin_url();

// `url` with its database replaced, as a key=value string.
[[nodiscard]] std::string with_database(const std::string& url, const std::string& database);

enum class Schema : std::uint8_t { Empty, Migrated };

// A database of its own for one test, dropped (sessions and all) on destruction, so tests
// never see each other's rows, locks or notifications.
class ScratchDatabase {
public:
    // Leaves `out` empty and skips the test when ULW_TEST_DATABASE_URL is unset and the
    // default server is unreachable; fails it when a configured server is unreachable.
    static void open(std::unique_ptr<ScratchDatabase>& out, Schema schema = Schema::Migrated);

    ScratchDatabase(infra::postgres::SyncConnection admin, std::string name, std::string conninfo);
    ~ScratchDatabase();
    ScratchDatabase(const ScratchDatabase&) = delete;
    ScratchDatabase& operator=(const ScratchDatabase&) = delete;
    ScratchDatabase(ScratchDatabase&&) = delete;
    ScratchDatabase& operator=(ScratchDatabase&&) = delete;

    [[nodiscard]] const std::string& conninfo() const noexcept { return conninfo_; }
    // A blocking session on this database, for arranging rows and checking them.
    [[nodiscard]] infra::postgres::SyncConnection session() const;

private:
    infra::postgres::SyncConnection admin_;
    std::string name_;
    std::string conninfo_;
};

struct ProcessResult {
    int exit_code = -1;
    std::string output;
};

// Runs a program found on PATH with `env` added to this process's environment, waits for it
// and returns its exit code and combined stdout and stderr.
[[nodiscard]] ProcessResult run_process(const std::vector<std::string>& argv,
                                        const std::vector<std::string>& env = {});

// The first column of the first row of `sql`, as text; "" for NULL or no rows.
[[nodiscard]] std::string scalar(infra::postgres::SyncConnection& conn, infra::postgres::Sql sql,
                                 const infra::postgres::Params& params = {});

} // namespace ulw::test
