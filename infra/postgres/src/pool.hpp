#pragma once

#include "net/offload_pool.hpp"
#include "net/reactor.hpp"

#include "conninfo.hpp"
#include "operation.hpp"
#include "timer.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

namespace infra::postgres {

struct PoolConfig {
    std::string conninfo;
    // Names the sessions in pg_stat_activity unless the connection string names them.
    std::string application_name;
    std::size_t connections = 1;
    core::Millis connect_timeout{};
    // From submission to the last result. The server's statement_timeout is set just below it,
    // so a statement the client stops waiting for does not keep its backend busy.
    core::Millis request_timeout{};
};

// libpq sessions driven by the reactor, never blocking it: libpq runs nonblocking, and host
// names are resolved on the offload pool. Operations start in submission order; each runs on
// one session from its first statement to its last.
// The offload pool must be stopped before this is destroyed, since a lookup may be running.
// Operations still outstanding are dropped without being told.
class Pool {
public:
    Pool(net::IReactor& reactor, net::OffloadPool& offload, PoolConfig config);
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
