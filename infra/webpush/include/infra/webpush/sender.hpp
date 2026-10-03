#pragma once

#include "core/ports/clock.hpp"
#include "core/util/time.hpp"
#include "infra/webpush/vapid.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// Delivery of encrypted push messages to push services (RFC 8030 section 5), on the reactor:
// a bounded queue, a bounded number of requests at once, a VAPID signature on each (RFC 8292),
// retries only where the push service asks for them (5xx and 429, after Retry-After), never past
// the message's own deadline. A subscription the push service says is gone (404, 410) is handed
// back to be forgotten.
namespace infra::webpush {

// RFC 8030 section 5.3.
enum class Urgency : std::uint8_t { VeryLow, Low, Normal, High };

[[nodiscard]] std::string_view to_string(Urgency urgency) noexcept;

// One POST, complete: the transport adds nothing but what HTTP itself needs.
struct PushRequest {
    std::string url;
    // Complete "name: value" lines.
    std::vector<std::string> headers;
    std::vector<std::uint8_t> body;
};

struct PushResponse {
    int status = 0;
    // Retry-After in delta-seconds, when the response carried it in that form.
    std::optional<core::Seconds> retry_after;
};

enum class PushFailure : std::uint8_t {
    // No response: the name did not resolve, the connection failed or broke, or timed out.
    Network,
    // The host resolved only to addresses a push may not go to (private, loopback, link-local).
    AddressRefused,
    // The push service's certificate was refused.
    Tls,
    // Nothing left this process: a malformed request, an allocation failure.
    Local,
};

using PushOutcome = std::expected<PushResponse, PushFailure>;

// Carries the POSTs: libcurl on the reactor in chat_server (CurlPushTransport), a fake in tests.
class IPushTransport {
public:
    // Called once, on the reactor thread, never from inside post().
    using Done = std::move_only_function<void(PushOutcome) noexcept>;
    virtual ~IPushTransport() = default;
    // False when the request could not be started; `done` is then never called. Requests still
    // running when the transport is destroyed are cancelled, their `done` never called.
    [[nodiscard]] virtual bool post(PushRequest request, Done done) noexcept = 0;
};

struct PushMessage {
    // A checked endpoint (check_endpoint) and its origin.
    std::string endpoint;
    std::string audience;
    // The encrypted body (encrypt()).
    std::vector<std::uint8_t> body;
    // Nothing is sent from this moment on: a ring's end. The TTL each attempt asks the push
    // service to keep the message for is what is left until then.
    core::MonoTime deadline;
    Urgency urgency = Urgency::Normal;
};

struct SenderLimits {
    // Messages waiting or between attempts. A ring pushes to each device of a callee (at most
    // ULW_PUSH_MAX_SUBSCRIPTIONS_PER_USER, 10 by default): 1024 is a hundred rings at once, each
    // message about 2.5 KiB at most with its endpoint, 2.5 MiB in all.
    std::size_t max_queue = 1'024;
    // POSTs at once; each holds a connection to a push service.
    std::size_t max_in_flight = 16;
    // Attempts per message, the first included.
    std::uint32_t max_attempts = 3;
    // The wait before a retry the push service gave no Retry-After for, doubling each time.
    core::Millis first_backoff{1'000};
    core::Millis max_backoff{8'000};
    // The VAPID subject: "mailto:" or "https://".
    std::string subject;
};

struct SenderCounters {
    std::uint64_t queued = 0;
    // 2xx: the push service took the message.
    std::uint64_t delivered = 0;
    // 404 or 410: the subscription is gone, and was handed back to be forgotten.
    std::uint64_t gone = 0;
    // Any other 4xx: the push service refused the message (a bad VAPID key, too large, ...).
    std::uint64_t rejected = 0;
    // 5xx or 429 on the last attempt allowed, or no response at all.
    std::uint64_t failed = 0;
    // The host resolved only to addresses a push may not go to.
    std::uint64_t refused = 0;
    // Attempts after the first.
    std::uint64_t retried = 0;
    // Reached their deadline waiting, unsent.
    std::uint64_t expired = 0;
    // Not queued: the queue was full.
    std::uint64_t dropped = 0;
};

// Everything runs on the reactor thread; time is the injected clock's. Sends start as soon as a
// place is free; retries wait for tick(), which the server runs after every turn of its loop.
class PushSender {
public:
    using Gone = std::move_only_function<void(const std::string& endpoint) noexcept>;

    PushSender(std::unique_ptr<IPushTransport> transport, const VapidKey& key,
               const core::ports::IClock& clock, SenderLimits limits);
    ~PushSender();
    PushSender(const PushSender&) = delete;
    PushSender& operator=(const PushSender&) = delete;

    // Where an endpoint the push service called gone goes; nothing by default.
    void on_gone(Gone gone) noexcept { gone_ = std::move(gone); }

    // False, and counted as dropped, when the queue is full.
    [[nodiscard]] bool enqueue(PushMessage message) noexcept;
    // Starts the retries that are due, and drops the messages past their deadline.
    void tick() noexcept;

    [[nodiscard]] const SenderCounters& counters() const noexcept { return counters_; }
    // Waiting to be sent, or between attempts.
    [[nodiscard]] std::size_t queued() const noexcept { return ready_.size() + waiting_.size(); }
    [[nodiscard]] std::size_t in_flight() const noexcept { return in_flight_; }

private:
    struct Pending {
        PushMessage message;
        std::uint32_t attempts = 0;
    };

    void pump() noexcept;
    void send(Pending pending) noexcept;
    void finished(Pending pending, PushOutcome outcome) noexcept;
    [[nodiscard]] std::optional<PushRequest> request_for(const Pending& pending,
                                                         core::Seconds ttl) const;

    const VapidKey& key_;
    const core::ports::IClock& clock_;
    SenderLimits limits_;
    SenderCounters counters_;
    Gone gone_;
    std::deque<Pending> ready_;
    // Retries, by when they are due.
    std::multimap<core::MonoTime, Pending> waiting_;
    std::size_t in_flight_ = 0;
    // Last: requests in flight are cancelled before anything their callbacks touch goes.
    std::unique_ptr<IPushTransport> transport_;
};

} // namespace infra::webpush
