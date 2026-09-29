#pragma once

#include "net/offload_pool.hpp"
#include "net/reactor.hpp"

#include "conninfo.hpp"
#include "operation.hpp"
#include "timer.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace infra::postgres {

// Called on the reactor thread.
class INotificationSink {
public:
    virtual ~INotificationSink() = default;
    // A session has just started listening. Anything sent while no session was listening is
    // lost, so whatever the notifications keep current must be reloaded.
    virtual void on_listening() noexcept = 0;
    // The payload is valid only during the call.
    virtual void on_notification(std::string_view payload) noexcept = 0;
};

struct PoolConfig {
    std::string conninfo;
    // Names the sessions in pg_stat_activity unless the connection string names them.
    std::string application_name;
    std::size_t connections = 1;
    core::Millis connect_timeout{};
    // From submission to the last result. The server's statement_timeout is set just below it,
    // so a statement the client stops waiting for does not keep its backend busy.
    core::Millis request_timeout{};
    // When set, every session runs this LISTEN as soon as it connects, before it takes any work,
    // and hands what arrives to `notifications`. One session is enough: each would deliver
    // every notification.
    std::optional<Sql> listen = std::nullopt;
    INotificationSink* notifications = nullptr;
};

// libpq sessions driven by the reactor, never blocking it: libpq runs nonblocking, and host
// names are resolved on the offload pool. Operations start in submission order; each runs on
// one session from its first statement to its last.
// The offload pool must be stopped before this is destroyed, since a lookup may be running.
// Operations still outstanding are dropped without being told.
class Pool {
    struct Token {
        explicit Token() = default;
    };

public:
    // Refuses a connection string ConnectPlan::create refuses, before any session starts.
    [[nodiscard]] static std::expected<std::unique_ptr<Pool>, std::string>
    create(net::IReactor& reactor, net::OffloadPool& offload, PoolConfig config);

    // Only create() can make the token.
    Pool(Token token, net::IReactor& reactor, net::OffloadPool& offload, PoolConfig config,
         ConnectPlan plan);
    ~Pool();
    Pool(const Pool&) = delete;
    Pool& operator=(const Pool&) = delete;
    Pool(Pool&&) = delete;
    Pool& operator=(Pool&&) = delete;

    // Never reports the operation's result from inside this call.
    void submit(std::unique_ptr<Operation> op);

    // Rises whenever an established session ends. Anything a session held (advisory locks)
    // while this read n is gone once it reads more.
    [[nodiscard]] std::uint64_t sessions_lost() const noexcept { return sessions_lost_; }

private:
    class Connection;
    struct Queued {
        std::unique_ptr<Operation> op;
        core::MonoTime deadline;
    };

    void dispatch() noexcept;
    void watch_deadlines() noexcept;
    void connection_idle(Connection& conn) noexcept;
    void connection_down(bool was_established) noexcept;
    [[nodiscard]] bool all_down() const noexcept;
    void fail_queued() noexcept;

    net::IReactor& reactor_;
    net::OffloadPool& offload_;
    PoolConfig config_;
    ConnectPlan plan_;
    std::deque<Queued> queue_;
    std::uint64_t sessions_lost_ = 0;
    Timer kick_;
    Timer watchdog_;
    // Last, so connections go first: they unwatch their sockets before anything else dies.
    std::vector<std::unique_ptr<Connection>> connections_;
};

} // namespace infra::postgres
