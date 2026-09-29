#include "pool.hpp"

#include "libpq_handles.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <utility>

namespace infra::postgres {

namespace {

// Serialization failures and deadlocks between our short transactions clear on the first
// rerun; the cap only stops a pathological loop.
constexpr int kMaxAttempts = 3;

// The reactor's timer wheel ticks every 100 ms; looking more often would fire nothing sooner.
constexpr core::Millis kWatchdogTick{100};

// Reconnects start a wheel tick after a failure and double up to 5 s: a restarted server is
// back within seconds, and waiting longer than that only stretches the outage.
constexpr core::Millis kFirstBackoff{100};
constexpr core::Millis kMaxBackoff{5000};

// A tick short of the client's deadline, the server cancels a statement that overran: the
// session survives that, whereas the client giving up first has to drop the connection.
core::Millis statement_timeout(core::Millis request_timeout) noexcept {
    // 0 would switch the server's timeout off altogether.
    return std::max(request_timeout - kWatchdogTick, core::Millis{1});
}

core::Millis backoff(unsigned failures) noexcept {
    // 100 ms << 6 already exceeds the cap.
    const unsigned doublings = std::min(failures, 7U) - 1;
    return std::min(kMaxBackoff, kFirstBackoff * (std::int64_t{1} << doublings));
}

} // namespace

class Pool::Connection final : public net::IReadyHandler, public net::IOffloadJob {
public:
    enum class State : std::uint8_t { Backoff, Resolving, Connecting, Idle, Busy };

    explicit Connection(Pool& pool)
        : pool_(pool), timer_(pool.reactor_, [this]() noexcept { on_timer(); }) {}
    ~Connection() override { close(); }
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;
    Connection(Connection&&) = delete;
    Connection& operator=(Connection&&) = delete;

    [[nodiscard]] State state() const noexcept { return state_; }

    void connect() noexcept {
        if (pool_.plan_.needs_lookup()) {
            // No connect timer yet: a getaddrinfo running on the offload pool cannot be called
            // off, and the resolver's own timeouts bound it.
            state_ = State::Resolving;
            pool_.offload_.submit(*this);
            return;
        }
        start_connect(nullptr);
    }

    void execute(Queued job) noexcept {
        op_ = std::move(job.op);
        deadline_ = job.deadline;
        attempts_ = 1;
        state_ = State::Busy;
        phase_ = Phase::Operation;
        send(op_->start());
    }

    void check_deadline(core::MonoTime now) noexcept {
        if (state_ == State::Busy && now >= deadline_) {
            // A cancel request would need a second, blocking connection. Closing the socket is
            // what the server notices instead; statement_timeout covers the rest.
            lose(DbError::Timeout);
        }
    }

    void on_ready(net::Interest ready) noexcept override {
        switch (state_) {
        case State::Connecting:
            advance_connect();
            return;
        case State::Idle:
            read_while_idle();
            return;
        case State::Busy:
            read_while_busy(ready);
            return;
        case State::Backoff:
        case State::Resolving:
            return;
        }
    }

    // On an offload thread; the reactor thread leaves resolved_ alone until complete().
    void run() noexcept override { resolved_ = pool_.plan_.resolve(); }

    void complete() noexcept override {
        std::optional<Endpoints> endpoints = std::exchange(resolved_, std::nullopt);
        if (!endpoints) {
            attempt_failed();
            return;
        }
        start_connect(&*endpoints);
    }

private:
    enum class Phase : std::uint8_t { Listen, Operation, Rollback };

    void start_connect(const Endpoints* endpoints) noexcept {
        state_ = State::Connecting;
        timer_.arm(pool_.config_.connect_timeout);
        const ConnectPlan::Arrays arrays = pool_.plan_.arrays(endpoints);
        conn_.reset(PQconnectStartParams(arrays.keywords.data(), arrays.values.data(), 1));
        if (!conn_ || PQstatus(conn_.get()) == CONNECTION_BAD ||
            PQsetnonblocking(conn_.get(), 1) != 0) {
            attempt_failed();
            return;
        }
        ignore_notices(conn_.get());
        // PQconnectStart has polled once already and is waiting to write.
        if (!watch(net::Interest::Write)) {
            attempt_failed();
        }
    }

    void advance_connect() noexcept {
        // PQconnectPoll may close the socket and open another (next address, SSL refused), and
        // the new one can reuse the old number, which a live registration would never notice.
        unwatch();
        switch (PQconnectPoll(conn_.get())) {
        case PGRES_POLLING_READING:
            if (!watch(net::Interest::Read)) {
                attempt_failed();
            }
            return;
        case PGRES_POLLING_WRITING:
            if (!watch(net::Interest::Write)) {
                attempt_failed();
            }
            return;
        case PGRES_POLLING_OK:
            timer_.cancel();
            failures_ = 0;
            if (!watch(net::Interest::Read)) {
                attempt_failed();
                return;
            }
            if (pool_.config_.listen) {
                start_listening(*pool_.config_.listen);
                return;
            }
            become_idle();
            return;
        case PGRES_POLLING_FAILED:
        case PGRES_POLLING_ACTIVE:
            attempt_failed();
            return;
        }
    }

    void read_while_idle() noexcept {
        if (PQconsumeInput(conn_.get()) == 0 || PQstatus(conn_.get()) == CONNECTION_BAD) {
            lose(DbError::ConnectionLost);
            return;
        }
        take_notifications();
    }

    // Also drains a session that does not LISTEN: a notification libpq has queued would
    // otherwise stay put.
    void take_notifications() noexcept {
        INotificationSink* const sink = pool_.config_.notifications;
        for (NotifyHandle n{PQnotifies(conn_.get())}; n; n.reset(PQnotifies(conn_.get()))) {
            if (sink != nullptr) {
                sink->on_notification(n->extra);
            }
        }
    }

    void read_while_busy(net::Interest ready) noexcept {
        if (net::has(ready, net::Interest::Read) && PQconsumeInput(conn_.get()) == 0) {
            lose(DbError::ConnectionLost);
            return;
        }
        take_notifications();
        if (writing_) {
            flush();
            if (state_ != State::Busy) {
                return;
            }
        }
        while (PQisBusy(conn_.get()) == 0) {
            ResultHandle next{PQgetResult(conn_.get())};
            if (!next) {
                statement_done();
                return;
            }
            // One statement yields one result; anything after it is dropped with `next`.
            if (!result_) {
                result_ = std::move(next);
            }
        }
    }

    void send(const Statement& statement) noexcept {
        const Params::Wire wire = statement.params.wire();
        if (PQsendQueryParams(conn_.get(), statement.sql.c_str(), wire.count, wire.types.data(),
                              wire.values.data(), wire.lengths.data(), wire.formats.data(),
                              0) == 0) {
            lose(error_of(nullptr, conn_.get()));
            return;
        }
        flush();
    }

    void flush() noexcept {
        const int rc = PQflush(conn_.get());
        if (rc < 0) {
            lose(DbError::ConnectionLost);
            return;
        }
        // Keep reading while the send buffer drains: the server may answer (or fail) early,
        // and a full receive buffer on our side would stall its writes and then ours.
        writing_ = rc == 1;
        const net::Interest want = writing_ ? net::Interest::ReadWrite : net::Interest::Read;
        if (want != interest_ && !watch(want)) {
            lose(DbError::ConnectionLost);
        }
    }

    void start_listening(const Sql& listen) noexcept {
        state_ = State::Busy;
        phase_ = Phase::Listen;
        deadline_ = pool_.reactor_.now() + pool_.config_.request_timeout;
        pool_.watchdog_.arm_unless_armed(kWatchdogTick);
        send(Statement{.sql = listen, .params = {}});
    }

    void statement_done() noexcept {
        ResultHandle raw = std::move(result_);
        const ExecStatusType status = raw ? PQresultStatus(raw.get()) : PGRES_FATAL_ERROR;
        const bool ok = status == PGRES_COMMAND_OK || status == PGRES_TUPLES_OK;
        const DbError error = ok ? DbError::Rejected : error_of(raw.get(), conn_.get());
        if (phase_ == Phase::Listen) {
            // A session that cannot listen is no use to its pool; the next one might.
            if (!ok) {
                lose(error);
                return;
            }
            phase_ = Phase::Operation;
            if (pool_.config_.notifications != nullptr) {
                pool_.config_.notifications->on_listening();
            }
            become_idle();
            return;
        }
        if (phase_ == Phase::Rollback) {
            if (!ok) {
                lose(error);
                return;
            }
            phase_ = Phase::Operation;
            if (op_) {
                send(op_->start());
                return;
            }
            become_idle();
            return;
        }
        if (!ok && error == DbError::ConnectionLost) {
            lose(error);
            return;
        }
        if (!ok && error == DbError::Retry && attempts_ < kMaxAttempts) {
            ++attempts_;
            if (PQtransactionStatus(conn_.get()) != PQTRANS_IDLE) {
                rollback();
                return;
            }
            send(op_->start());
            return;
        }
        Outcome outcome = ok ? Outcome{Result{std::move(raw)}} : Outcome{std::unexpected(error)};
        const std::optional<Statement> next = op_->next(std::move(outcome));
        if (next) {
            send(*next);
            return;
        }
        op_.reset();
        if (PQtransactionStatus(conn_.get()) != PQTRANS_IDLE) {
            deadline_ = pool_.reactor_.now() + pool_.config_.request_timeout;
            rollback();
            return;
        }
        become_idle();
    }

    void rollback() noexcept {
        phase_ = Phase::Rollback;
        send(Statement{.sql = "ROLLBACK", .params = {}});
    }

    void become_idle() noexcept {
        state_ = State::Idle;
        if (interest_ != net::Interest::Read && !watch(net::Interest::Read)) {
            lose(DbError::ConnectionLost);
            return;
        }
        pool_.connection_idle(*this);
    }

    [[nodiscard]] bool watch(net::Interest interest) noexcept {
        const int fd = PQsocket(conn_.get());
        if (fd < 0) {
            return false;
        }
        if (!pool_.reactor_.watch(fd, interest, *this)) {
            return false;
        }
        fd_ = fd;
        interest_ = interest;
        return true;
    }

    void unwatch() noexcept {
        if (fd_ >= 0) {
            pool_.reactor_.unwatch(fd_);
            fd_ = -1;
            interest_ = net::Interest::None;
        }
    }

    // The reactor must forget the socket before libpq closes it.
    void close() noexcept {
        unwatch();
        conn_.reset();
        result_.reset();
        writing_ = false;
    }

    void enter_backoff() noexcept {
        close();
        state_ = State::Backoff;
        ++failures_;
        timer_.arm(backoff(failures_));
    }

    void attempt_failed() noexcept {
        enter_backoff();
        pool_.connection_down(false);
    }

    void lose(DbError why) noexcept {
        const bool established = state_ == State::Idle || state_ == State::Busy;
        std::unique_ptr<Operation> op = std::move(op_);
        enter_backoff();
        pool_.connection_down(established);
        if (op) {
            op->abandon(why);
        }
    }

    void on_timer() noexcept {
        switch (state_) {
        case State::Connecting:
            attempt_failed();
            return;
        case State::Backoff:
            connect();
            return;
        case State::Resolving:
        case State::Idle:
        case State::Busy:
            return;
        }
    }

    Pool& pool_;
    // Connect timeout while connecting; reconnect delay while down.
    Timer timer_;
    ConnHandle conn_;
    int fd_ = -1;
    net::Interest interest_ = net::Interest::None;
    State state_ = State::Backoff;
    Phase phase_ = Phase::Operation;
    bool writing_ = false;
    unsigned failures_ = 0;
    int attempts_ = 0;
    std::unique_ptr<Operation> op_;
    core::MonoTime deadline_;
    ResultHandle result_;
    std::optional<Endpoints> resolved_;
};

std::expected<std::unique_ptr<Pool>, std::string>
Pool::create(net::IReactor& reactor, net::OffloadPool& offload, PoolConfig config) {
    auto plan = ConnectPlan::create(config.conninfo, config.application_name,
                                    statement_timeout(config.request_timeout));
    if (!plan) {
        return std::unexpected(std::move(plan.error()));
    }
    return std::make_unique<Pool>(Token{}, reactor, offload, std::move(config), std::move(*plan));
}

Pool::Pool(Token /*token*/, net::IReactor& reactor, net::OffloadPool& offload, PoolConfig config,
           ConnectPlan plan)
    : reactor_(reactor), offload_(offload), config_(std::move(config)), plan_(std::move(plan)),
      kick_(reactor, [this]() noexcept { dispatch(); }),
      watchdog_(reactor, [this]() noexcept { watch_deadlines(); }) {
    connections_.reserve(config_.connections);
    for (std::size_t i = 0; i < config_.connections; ++i) {
        connections_.push_back(std::make_unique<Connection>(*this));
    }
    for (const auto& conn : connections_) {
        conn->connect();
    }
}

Pool::~Pool() = default;

void Pool::submit(std::unique_ptr<Operation> op) {
    queue_.push_back(
        Queued{.op = std::move(op), .deadline = reactor_.now() + config_.request_timeout});
    // Starting it from here could fail it from here, inside the caller's call.
    kick_.arm_unless_armed(core::Millis{0});
    watchdog_.arm_unless_armed(kWatchdogTick);
}

void Pool::dispatch() noexcept {
    for (const auto& conn : connections_) {
        if (queue_.empty()) {
            return;
        }
        if (conn->state() == Connection::State::Idle) {
            Queued job = std::move(queue_.front());
            queue_.pop_front();
            conn->execute(std::move(job));
        }
    }
    if (!queue_.empty() && all_down()) {
        fail_queued();
    }
}

void Pool::connection_idle(Connection& conn) noexcept {
    if (queue_.empty()) {
        return;
    }
    Queued job = std::move(queue_.front());
    queue_.pop_front();
    conn.execute(std::move(job));
}

void Pool::connection_down(bool was_established) noexcept {
    if (was_established) {
        ++sessions_lost_;
    }
    // Nothing is connected or connecting: the server is known to be unreachable, and queued
    // work would only sit out its deadline.
    if (all_down()) {
        fail_queued();
    }
}

bool Pool::all_down() const noexcept {
    return std::ranges::all_of(
        connections_, [](const auto& conn) { return conn->state() == Connection::State::Backoff; });
}

void Pool::fail_queued() noexcept {
    std::deque<Queued> doomed;
    doomed.swap(queue_);
    for (Queued& job : doomed) {
        job.op->abandon(DbError::ConnectionLost);
    }
}

void Pool::watch_deadlines() noexcept {
    const core::MonoTime now = reactor_.now();
    // Every operation gets the same timeout, so deadlines rise along the queue.
    while (!queue_.empty() && queue_.front().deadline <= now) {
        Queued job = std::move(queue_.front());
        queue_.pop_front();
        job.op->abandon(DbError::Timeout);
    }
    for (const auto& conn : connections_) {
        conn->check_deadline(now);
    }
    const bool busy = std::ranges::any_of(
        connections_, [](const auto& conn) { return conn->state() == Connection::State::Busy; });
    if (busy || !queue_.empty()) {
        watchdog_.arm(kWatchdogTick);
    }
}

} // namespace infra::postgres
