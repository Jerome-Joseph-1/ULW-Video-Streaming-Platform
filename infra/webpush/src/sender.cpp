#include "infra/webpush/sender.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <new>
#include <utility>

namespace infra::webpush {

namespace {

constexpr int kNotFound = 404;
constexpr int kGone = 410;
constexpr int kTooManyRequests = 429;

bool retryable(int status) noexcept {
    return status == kTooManyRequests || (status >= 500 && status <= 599);
}

} // namespace

std::string_view to_string(Urgency urgency) noexcept {
    switch (urgency) {
    case Urgency::VeryLow:
        return "very-low";
    case Urgency::Low:
        return "low";
    case Urgency::Normal:
        return "normal";
    case Urgency::High:
        return "high";
    }
    return "normal";
}

PushSender::PushSender(std::unique_ptr<IPushTransport> transport, const VapidKey& key,
                       const core::ports::IClock& clock, core::ports::IRandom& random,
                       SenderLimits limits)
    : key_(key), clock_(clock), random_(random), limits_(std::move(limits)),
      transport_(std::move(transport)) {}

std::optional<std::string> PushSender::authorization(const std::string& audience) {
    const core::WallTime now = clock_.wall_now();
    if (const auto it = signed_.find(audience); it != signed_.end() && now < it->second.renew_at) {
        return it->second.header;
    }
    auto header = key_.authorization(audience, limits_.subject, now + kVapidLifetime);
    if (!header) {
        return std::nullopt;
    }
    ++counters_.signatures;
    if (signed_.size() >= kMaxAudiences && !signed_.contains(audience)) {
        signed_.clear();
    }
    signed_.insert_or_assign(
        audience,
        Signed{.header = *header, .renew_at = now + kVapidLifetime - kVapidRefreshBefore});
    return std::move(*header);
}

core::Millis PushSender::backoff(std::uint32_t attempts) noexcept {
    core::Millis wait = limits_.first_backoff;
    for (std::uint32_t i = 1; i < attempts && wait < limits_.max_backoff; ++i) {
        wait = std::min(wait * 2, limits_.max_backoff);
    }
    // Half of it, and a random part of the other half.
    std::array<std::byte, sizeof(std::uint64_t)> bytes{};
    random_.fill(bytes);
    const auto half = static_cast<std::uint64_t>(wait.count() / 2);
    const std::uint64_t draw = std::bit_cast<std::uint64_t>(bytes) % (half + 1);
    return core::Millis{static_cast<core::Millis::rep>(half + draw)};
}

PushSender::~PushSender() {
    // The requests in flight go first, before the queues their callbacks would touch.
    transport_.reset();
}

bool PushSender::enqueue(PushMessage message) noexcept {
    if (queued() >= limits_.max_queue) {
        ++counters_.dropped;
        return false;
    }
    try {
        ready_.push_back(Pending{.message = std::move(message), .attempts = 0});
    } catch (const std::bad_alloc&) {
        ++counters_.dropped;
        return false;
    }
    ++counters_.queued;
    pump();
    return true;
}

void PushSender::tick() noexcept {
    const core::MonoTime now = clock_.now();
    while (!waiting_.empty() && waiting_.begin()->first <= now) {
        auto node = waiting_.extract(waiting_.begin());
        try {
            ready_.push_back(std::move(node.mapped()));
        } catch (const std::bad_alloc&) {
            ++counters_.failed;
        }
    }
    pump();
}

void PushSender::pump() noexcept {
    while (in_flight_ < limits_.max_in_flight && !ready_.empty()) {
        Pending next = std::move(ready_.front());
        ready_.pop_front();
        if (clock_.now() >= next.message.deadline) {
            ++counters_.expired;
            continue;
        }
        send(std::move(next));
    }
}

std::optional<PushRequest> PushSender::request_for(const Pending& pending, core::Seconds ttl) {
    const PushMessage& m = pending.message;
    auto authorization = this->authorization(m.audience);
    if (!authorization) {
        return std::nullopt;
    }
    PushRequest request{.url = m.endpoint, .headers = {}, .body = m.body};
    request.headers.reserve(5);
    request.headers.push_back("TTL: " + std::to_string(ttl.count()));
    request.headers.push_back("Urgency: " + std::string(to_string(m.urgency)));
    request.headers.emplace_back("Content-Encoding: aes128gcm");
    request.headers.emplace_back("Content-Type: application/octet-stream");
    request.headers.push_back("Authorization: " + *authorization);
    return request;
}

void PushSender::send(Pending pending) noexcept {
    try {
        // What is left of the message's life, in whole seconds, never 0 (which asks the push
        // service to deliver now or never).
        const auto left = std::chrono::ceil<core::Seconds>(pending.message.deadline - clock_.now());
        auto request = request_for(pending, std::max(left, core::Seconds{1}));
        if (!request) {
            ++counters_.failed;
            return;
        }
        ++pending.attempts;
        ++in_flight_;
        const bool started =
            transport_->post(std::move(*request),
                             [this, p = std::move(pending)](PushOutcome outcome) mutable noexcept {
                                 finished(std::move(p), std::move(outcome));
                             });
        if (!started) {
            --in_flight_;
            ++counters_.failed;
        }
    } catch (const std::bad_alloc&) {
        ++counters_.failed;
    }
}

void PushSender::finished(Pending pending, PushOutcome outcome) noexcept {
    --in_flight_;
    if (!outcome) {
        switch (outcome.error()) {
        case PushFailure::AddressRefused:
            ++counters_.refused;
            break;
        case PushFailure::Network:
        case PushFailure::Tls:
        case PushFailure::Local:
            ++counters_.failed;
            break;
        }
        pump();
        return;
    }
    const int status = outcome->status;
    if (status >= 200 && status <= 299) {
        ++counters_.delivered;
    } else if (status == kNotFound || status == kGone) {
        ++counters_.gone;
        if (gone_) {
            gone_(pending.message.endpoint);
        }
    } else if (retryable(status)) {
        const core::MonoTime now = clock_.now();
        core::Millis wait = backoff(pending.attempts);
        bool never = false;
        if (const std::optional<core::Seconds> after = outcome->retry_after) {
            never = *after > core::Seconds{kMaxRetryAfter};
            wait = std::max<core::Millis>(wait, *after);
        }
        if (never || pending.attempts >= limits_.max_attempts ||
            now + wait >= pending.message.deadline) {
            ++counters_.failed;
        } else {
            try {
                waiting_.emplace(now + wait, std::move(pending));
                ++counters_.retried;
            } catch (const std::bad_alloc&) {
                ++counters_.failed;
            }
        }
    } else {
        ++counters_.rejected;
    }
    pump();
}

} // namespace infra::webpush
