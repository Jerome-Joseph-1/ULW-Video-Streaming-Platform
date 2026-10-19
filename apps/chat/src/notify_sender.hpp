#pragma once

#include "core/ports/clock.hpp"
#include "infra/curl/multi.hpp"
#include "net/reactor.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

// The notification hook's way out (ADR-0111): events POSTed to the operator's URL, best effort.
namespace chat {

// Where events go: the sender, or a fake in tests. offer() never blocks and never fails the
// caller: an event it cannot take is dropped and counted.
class IEventSink {
public:
    virtual ~IEventSink() = default;
    // `event` is one JSON object.
    virtual void offer(std::string event) noexcept = 0;
};

struct SenderLimits {
    // Events waiting to be sent. A ring is a handful of events and a busy room's message one
    // every interval per member; 4096 is minutes of a node's events with the receiver down.
    std::size_t max_queued = 4'096;
    // Events per request.
    std::size_t batch = 64;
    // A request not answered within this is given up, its events dropped: a ring is over in
    // 45 s, and a late push is worse than none.
    core::Millis timeout{2'000};
};

struct SenderCounters {
    // Events in requests the receiver answered 2xx.
    std::uint64_t sent = 0;
    std::uint64_t requests = 0;
    // Events dropped because the queue was full, or because their request failed (no 2xx, a
    // timeout, a network error).
    std::uint64_t dropped = 0;
    std::uint64_t failed = 0;
    std::uint64_t failed_requests = 0;
};

// "t=<unix seconds>,v1=<hex HMAC-SHA256 of "<t>.<body>" under `secret`>": the ULW-Signature
// header's value.
[[nodiscard]] std::string notify_signature(std::string_view secret, std::int64_t unix_seconds,
                                           std::string_view body);

// One request at a time, so a node's events arrive in the order it made them; each request
// carries up to SenderLimits::batch events as {"events":[...]}, signed. Nothing is retried: a
// request that fails drops its events (at most once). Reactor thread only.
class HookSender final : public IEventSink {
public:
    HookSender(net::IReactor& reactor, infra::curl::Multi& multi, const core::ports::IClock& clock,
               std::string url, std::string secret, SenderLimits limits = {});
    ~HookSender() override;
    HookSender(const HookSender&) = delete;
    HookSender& operator=(const HookSender&) = delete;
    HookSender(HookSender&&) = delete;
    HookSender& operator=(HookSender&&) = delete;

    void offer(std::string event) noexcept override;

    [[nodiscard]] const SenderCounters& counters() const noexcept { return counters_; }
    [[nodiscard]] std::size_t queued() const noexcept { return queue_.size(); }
    [[nodiscard]] bool busy() const noexcept { return request_ != nullptr; }

private:
    class Request;

    void start() noexcept;
    // `status`: the receiver's HTTP status, or nullopt when there was no answer.
    void finished(std::optional<int> status) noexcept;

    net::IReactor& reactor_;
    infra::curl::Multi& multi_;
    const core::ports::IClock& clock_;
    std::string url_;
    std::string secret_;
    SenderLimits limits_;
    SenderCounters counters_;
    std::deque<std::string> queue_;
    std::unique_ptr<Request> request_;
};

} // namespace chat
