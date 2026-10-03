#pragma once

#include "core/ports/clock.hpp"
#include "core/util/time.hpp"
#include "net/reactor.hpp"
#include "os/unique_fd.hpp"

#include "livekit_webhook.hpp"
#include "ops/log.hpp"
#include "publisher_watch.hpp"
#include "rate_limit.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

namespace gateway {

// The one path the webhook listener serves.
inline constexpr std::string_view kWebhookPath = "/livekit/webhook";

struct WebhookLimits {
    // LiveKit posts from one server, one request at a time per room and a handful of rooms at
    // once; anything past this many connections is not LiveKit.
    std::size_t max_connections = 32;
    // An idle keep-alive connection, or a request that stalls, is closed after this.
    core::Millis idle_timeout{10'000};
    // Requests a second, over all connections, and how many may come at once. LiveKit sends a
    // few events per participant and room; a busy hour of calls is tens a second at most, and
    // a refusal (429) is something LiveKit retries.
    BucketRule rate{.burst = 200, .per_second = 100};
};

struct WebhookCounters {
    std::uint64_t connections = 0;
    // Closed on accept: max_connections were open.
    std::uint64_t refused_connections = 0;
    std::uint64_t requests = 0;
    // Verified, read and handed on.
    std::uint64_t accepted = 0;
    // By WebhookRejection.
    std::array<std::uint64_t, kWebhookRejections> refused{};
    // Over the rate.
    std::uint64_t limited = 0;
    // Another path or method, a body that is not an event, a request HTTP cannot parse.
    std::uint64_t bad_requests = 0;
};

// LiveKit's webhooks, on a listener of their own that the public route never reaches
// (ADR-0093): plain HTTP/1.1, POST kWebhookPath only, each request verified with LiveKit's
// key pair before its body is read as an event and handed to the sink. A verified event is
// answered 200 at once and acted on afterwards: LiveKit sends a room's events one at a time and
// drops what waits longer than 30 s, so nothing here waits for the stream service.
//
// Reactor thread only.
class WebhookServer final : public net::IAcceptHandler {
public:
    WebhookServer(net::IReactor& reactor, const core::ports::IClock& clock, WebhookKey key,
                  IWebhookSink& sink, ops::Logger& log, WebhookLimits limits);
    ~WebhookServer() override;
    WebhookServer(const WebhookServer&) = delete;
    WebhookServer& operator=(const WebhookServer&) = delete;

    void on_accept(os::UniqueFd conn) noexcept override;
    // Destroys connections the reactor has let go of. Call after each run_once.
    void reap() noexcept;

    [[nodiscard]] const WebhookCounters& counters() const noexcept { return counters_; }
    [[nodiscard]] std::size_t connections() const noexcept { return connections_.size(); }

private:
    class Connection;

    net::IReactor& reactor_;
    const core::ports::IClock& clock_;
    WebhookKey key_;
    IWebhookSink& sink_;
    ops::Logger& log_;
    WebhookLimits limits_;
    WebhookCounters counters_;
    TokenBucket bucket_;
    std::vector<std::unique_ptr<Connection>> connections_;
};

} // namespace gateway
