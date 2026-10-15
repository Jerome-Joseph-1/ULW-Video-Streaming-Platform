#pragma once

#include "core/ports/notify.hpp"
#include "net/offload_pool.hpp"
#include "net/reactor.hpp"

#include <cstddef>
#include <expected>
#include <memory>
#include <string>

namespace infra::postgres {

struct NotifyStoreConfig {
    std::string conninfo;
    // A node renews three times a minute, writes its sockets' changes one statement at a time,
    // and asks one statement per ring and per message: 2 sessions carry that.
    std::size_t connections = 2;
    core::Millis connect_timeout{5000};
    // Every statement touches a handful of rows by key. Past 2 s the caller has moved on: a
    // ring's event goes out without devices, a message's without anyone.
    core::Millis request_timeout{2000};
    core::ports::NotifyTiming timing;
};

// The connectivity registry on Postgres (ADR-0111, migration 0022), driven by the reactor. Time
// is the database's: a lease is measured against now() on the server, so nodes' clocks never
// disagree about it.
//
// message_targets() is one statement: it reads who is owed a notification and marks them told,
// under the marks' row locks, so two nodes asking at once answer each member once.
//
// The offload pool resolves host names and must be stopped before this is destroyed. Calls
// still outstanding at destruction are dropped without their callbacks.
class PgNotifyStore final : public core::ports::IConnectivityStore {
    class Impl;
    struct Token {
        explicit Token() = default;
    };

public:
    [[nodiscard]] static std::expected<std::unique_ptr<PgNotifyStore>, std::string>
    create(net::IReactor& reactor, net::OffloadPool& offload, const NotifyStoreConfig& config);

    // Only create() can make the token.
    PgNotifyStore(Token token, std::unique_ptr<Impl> impl) noexcept;
    ~PgNotifyStore() override;
    PgNotifyStore(const PgNotifyStore&) = delete;
    PgNotifyStore& operator=(const PgNotifyStore&) = delete;
    PgNotifyStore(PgNotifyStore&&) = delete;
    PgNotifyStore& operator=(PgNotifyStore&&) = delete;

    void renew(std::uint64_t node, core::ports::NotifyCallback<bool> done) override;
    void sync(std::uint64_t node, std::vector<core::ports::SocketDevice> added,
              std::vector<core::ports::SocketDevice> removed,
              core::ports::NotifyCallback<void> done) override;
    void forget(std::uint64_t node, core::ports::NotifyCallback<void> done) override;
    void
    connected(std::vector<core::UserId> users,
              core::ports::NotifyCallback<std::vector<core::ports::SocketDevice>> done) override;
    void message_targets(const core::RoomId& room, const core::UserId& sender,
                         core::ports::NotifyCallback<std::vector<core::UserId>> done) override;

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace infra::postgres
