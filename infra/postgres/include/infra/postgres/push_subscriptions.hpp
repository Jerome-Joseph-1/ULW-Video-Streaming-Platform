#pragma once

#include "core/ports/push_subscriptions.hpp"
#include "net/offload_pool.hpp"
#include "net/reactor.hpp"

#include <cstddef>
#include <expected>
#include <memory>
#include <string>

namespace infra::postgres {

struct PushSubscriptionsConfig {
    std::string conninfo;
    // A save when a client subscribes, a read per callee when a call starts ringing, a delete
    // when a push service says a subscription is gone: a trickle next to messages. Two sessions
    // keep one slow statement from holding up a ring.
    std::size_t connections = 2;
    core::Millis connect_timeout{5000};
    // Every statement reads or writes a user's handful of rows by key.
    core::Millis request_timeout{2000};
};

// IPushSubscriptions on Postgres (migrations/0015_push_subscriptions.sql), driven by the reactor.
// Endpoints are capabilities: neither they nor anything derived from them reaches a log or an
// error. The offload pool resolves host names and must be stopped before this is destroyed. Calls
// outstanding at destruction are dropped unanswered.
class PgPushSubscriptions final : public core::ports::IPushSubscriptions {
    class Impl;
    struct Token {
        explicit Token() = default;
    };

public:
    [[nodiscard]] static std::expected<std::unique_ptr<PgPushSubscriptions>, std::string>
    create(net::IReactor& reactor, net::OffloadPool& offload,
           const PushSubscriptionsConfig& config);

    PgPushSubscriptions(Token token, std::unique_ptr<Impl> impl) noexcept;
    ~PgPushSubscriptions() override;
    PgPushSubscriptions(const PgPushSubscriptions&) = delete;
    PgPushSubscriptions& operator=(const PgPushSubscriptions&) = delete;

    void save(const core::UserId& user, const core::ports::PushSubscription& subscription,
              std::size_t max_per_user, core::ports::PushCallback<void> done) override;
    void remove(const core::UserId& user, const core::DeviceId& device,
                core::ports::PushCallback<void> done) override;
    void list(const core::UserId& user, std::size_t limit,
              core::ports::PushCallback<std::vector<core::ports::PushSubscription>> done) override;
    void forget(const std::string& endpoint, core::ports::PushCallback<void> done) override;

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace infra::postgres
