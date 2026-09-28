#pragma once

#include "core/ports/catalog.hpp"
#include "net/reactor.hpp"

#include "support/reactor_harness.hpp"
#include "sync_connection.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
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

// The container of ULW_TEST_PG_CONTAINER (default ulw-pg) when docker can control it.
[[nodiscard]] std::optional<std::string> postgres_container();

// docker pause for as long as it lives: the server stops answering while every socket to it
// stays open, which is what a hung or partitioned database looks like from here.
class PausedServer {
public:
    explicit PausedServer(std::string container);
    ~PausedServer();
    PausedServer(const PausedServer&) = delete;
    PausedServer& operator=(const PausedServer&) = delete;
    PausedServer(PausedServer&&) = delete;
    PausedServer& operator=(PausedServer&&) = delete;

    [[nodiscard]] bool paused() const noexcept { return paused_; }

private:
    std::string container_;
    bool paused_ = false;
};

// The result a catalog call delivers, once the reactor has delivered it.
template <class T> class Reply {
public:
    [[nodiscard]] core::ports::CatalogCallback<T> callback() {
        return [this](core::ports::CatalogResult<T> r) noexcept {
            ++calls_;
            result_ = std::move(r);
        };
    }
    [[nodiscard]] bool ready() const noexcept { return result_.has_value(); }
    [[nodiscard]] int calls() const noexcept { return calls_; }
    [[nodiscard]] const core::ports::CatalogResult<T>& get() const { return *result_; }

private:
    std::optional<core::ports::CatalogResult<T>> result_;
    int calls_ = 0;
};

// Pumps until the reply arrives; a reply that never comes fails the test.
template <class T>
core::ports::CatalogResult<T> wait(net::IReactor& reactor, Reply<T>& reply,
                                   std::chrono::milliseconds limit = std::chrono::seconds(15)) {
    if (!pump_until(reactor, [&] { return reply.ready(); }, limit)) {
        ADD_FAILURE() << "catalog call never answered";
        return std::unexpected(core::ports::CatalogError::Unavailable);
    }
    return reply.get();
}

// The first column of the first row of `sql`, as text; "" for NULL or no rows.
[[nodiscard]] std::string scalar(infra::postgres::SyncConnection& conn, infra::postgres::Sql sql,
                                 const infra::postgres::Params& params = {});

} // namespace ulw::test
