#include "infra/auth/jwks_verifier.hpp"

#include "core/ports/auth.hpp"
#include "core/util/time.hpp"
#include "net/reactor.hpp"

#include "claims.hpp"
#include "jwk.hpp"
#include "jws.hpp"
#include "result_cache.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace infra::auth {

using core::ports::AuthError;
using core::ports::IKeyWaiter;
using core::ports::VerifyResult;

namespace {

// How long a key withdrawn from the set goes on verifying here, and how long a verdict is
// reused. A quarter of an hour keeps both well inside the lifetime of an ordinary access
// token for the price of one small fetch per process.
constexpr core::Millis kKeyLifetime = std::chrono::minutes(15);
constexpr core::Millis kVerdictLifetime = kKeyLifetime;

// A kid still missing after a fetch is refused without another fetch for this long: a client
// replaying one stale token costs a fetch a minute at most, and a key published just after
// the fetch that missed it is not locked out for longer.
constexpr core::Millis kUnknownKidMemory = std::chrono::seconds(60);

// Fetches for unseen kids are at least this far apart, which caps what junk kids can cost the
// JWKS endpoint at six fetches a minute per process. A genuinely new key is refused only if
// it turns up within this long of a fetch that did not have it.
constexpr core::Millis kUnseenKidFetchSpacing = std::chrono::seconds(10);

// Doubling from 1 s up to a minute: a recovered endpoint is noticed within a minute, and a
// down one sees a request a minute from each process.
constexpr core::Millis kFirstRetry = std::chrono::seconds(1);
constexpr core::Millis kMaxRetry = std::chrono::minutes(1);

// Real traffic presents one kid, or two while keys rotate; past a few dozen they are junk,
// which the fetch spacing already throttles.
constexpr std::size_t kMaxRememberedKids = 64;

core::Millis retry_delay(std::uint32_t failures) noexcept {
    // 1 s << 6 is the first delay past the cap.
    const std::uint32_t doublings = std::min<std::uint32_t>(failures - 1, 6);
    return std::min(kMaxRetry, kFirstRetry * (std::int64_t{1} << doublings));
}

} // namespace

class JwksVerifier::State final : public IKeySetReceiver {
public:
    State(net::IReactor& reactor, IKeySetFetcher& fetcher, JwksConfig config)
        : reactor_(reactor), fetcher_(fetcher), config_(std::move(config)) {}

    ~State() override {
        if (fetching_) {
            fetcher_.cancel(*this);
        }
        cancel(refetch_timer_);
        cancel(notify_timer_);
    }

    State(const State&) = delete;
    State& operator=(const State&) = delete;
    State(State&&) = delete;
    State& operator=(State&&) = delete;

    std::optional<VerifyResult> verify(std::string_view token, core::WallTime now,
                                       IKeyWaiter& waiter) {
        if (token.empty() || token.size() > detail::kMaxTokenBytes) {
            return VerifyResult{std::unexpected(AuthError::Malformed)};
        }
        const core::MonoTime mono = reactor_.now();
        expire_stale_keys(mono);
        const std::optional<detail::TokenDigest> digest = detail::digest_token(token);
        if (digest) {
            if (std::optional<core::ports::Claims> hit = verdicts_.find(*digest, mono, now)) {
                return VerifyResult{std::move(*hit)};
            }
        }
        const auto jws = detail::parse_compact(token);
        if (!jws) {
            return VerifyResult{std::unexpected(jws.error())};
        }
        const detail::PublicKey* key = keys_.find(jws->kid);
        if (key == nullptr) {
            return on_unseen_kid(jws->kid, waiter, mono);
        }
        VerifyResult result = detail::authenticate(*jws, *key, config_.claims, now);
        if (result && digest) {
            verdicts_.insert(*digest, *result, mono + kVerdictLifetime);
        }
        return result;
    }

    [[nodiscard]] bool keys_expired() const noexcept { return expired_; }

    void cancel_wait(IKeyWaiter& waiter) noexcept {
        std::erase(waiters_, &waiter);
        std::ranges::replace(notifying_, &waiter, nullptr);
    }

    void on_key_set(std::optional<std::string_view> body) noexcept override {
        if (!fetching_) {
            return;
        }
        fetching_ = false;
        const core::MonoTime now = reactor_.now();
        last_fetch_end_ = now;
        std::optional<detail::KeySet> set =
            body ? detail::parse_key_set(*body) : std::optional<detail::KeySet>{};
        // A set without one usable key is likelier a publishing mistake than the withdrawal
        // of every key, so it counts as a failed fetch and the keys in hand stay.
        if (set && !set->keys.empty()) {
            install(std::move(*set), now);
            failed_fetches_ = 0;
            fetched_at_ = now;
            expired_ = false;
            schedule_refetch(kKeyLifetime);
        } else {
            ++failed_fetches_;
            expire_stale_keys(now);
            schedule_refetch(retry_delay(failed_fetches_));
        }
        sought_kids_.clear();
        if (!waiters_.empty() && !notify_timer_) {
            // Zero-delay timers run on the next iteration, so no waiter is called from inside
            // this callback, which a fetcher may make from inside fetch() and so from verify().
            notify_timer_ = reactor_.arm_timer(core::Millis{0}, notify_due_);
        }
    }

private:
    struct UnknownKid {
        std::string kid;
        core::MonoTime until;
    };

    // The keys and every verdict they produced go once the last successful fetch is
    // max_key_age old; what is left refuses every token until a fetch succeeds.
    void expire_stale_keys(core::MonoTime now) noexcept {
        if (expired_ || !fetched_at_ || now - *fetched_at_ < config_.max_key_age) {
            return;
        }
        expired_ = true;
        keys_ = detail::KeySet{};
        verdicts_.clear();
        if (config_.on_keys_expired) {
            config_.on_keys_expired(std::chrono::duration_cast<core::Millis>(now - *fetched_at_));
        }
    }

    std::optional<VerifyResult> on_unseen_kid(std::string_view kid, IKeyWaiter& waiter,
                                              core::MonoTime now) {
        if (remembered_unknown(kid, now)) {
            return VerifyResult{std::unexpected(AuthError::UnknownKey)};
        }
        if (!fetching_) {
            // The retry timer is already set; answering at once beats queueing behind it.
            if (failed_fetches_ > 0) {
                return VerifyResult{std::unexpected(AuthError::KeysUnavailable)};
            }
            if (last_fetch_end_ && now - *last_fetch_end_ < kUnseenKidFetchSpacing) {
                return VerifyResult{std::unexpected(AuthError::UnknownKey)};
            }
        }
        // Registered before the fetch starts, in case it completes inside fetch().
        if (std::ranges::find(waiters_, &waiter) == waiters_.end()) {
            waiters_.push_back(&waiter);
        }
        if (sought_kids_.size() < kMaxRememberedKids &&
            std::ranges::find(sought_kids_, kid) == sought_kids_.end()) {
            sought_kids_.emplace_back(kid);
        }
        if (!fetching_) {
            start_fetch();
        }
        return std::nullopt;
    }

    void install(detail::KeySet set, core::MonoTime now) {
        const bool withdrawn = std::ranges::any_of(keys_.keys, [&](const detail::PublicKey& old) {
            const detail::PublicKey* current = set.find(old.kid);
            return current == nullptr || !detail::same_key(old, *current);
        });
        // A verdict must not outlive the key that produced it.
        if (withdrawn) {
            verdicts_.clear();
        }
        for (const std::string& kid : sought_kids_) {
            if (set.find(kid) == nullptr) {
                remember_unknown(kid, now + kUnknownKidMemory);
            }
        }
        keys_ = std::move(set);
    }

    // Every entry lives equally long, so the deque is in expiry order.
    void remember_unknown(std::string_view kid, core::MonoTime until) {
        if (unknown_kids_.size() == kMaxRememberedKids) {
            unknown_kids_.pop_front();
        }
        unknown_kids_.push_back({.kid = std::string(kid), .until = until});
    }

    bool remembered_unknown(std::string_view kid, core::MonoTime now) noexcept {
        while (!unknown_kids_.empty() && unknown_kids_.front().until <= now) {
            unknown_kids_.pop_front();
        }
        return std::ranges::any_of(unknown_kids_,
                                   [&](const UnknownKid& u) { return u.kid == kid; });
    }

    void start_fetch() noexcept {
        fetching_ = true;
        cancel(refetch_timer_);
        fetcher_.fetch(config_.url, *this);
    }

    void schedule_refetch(core::Millis delay) {
        cancel(refetch_timer_);
        refetch_timer_ = reactor_.arm_timer(delay, refetch_due_);
    }

    void on_refetch_due() noexcept {
        refetch_timer_.reset();
        if (!fetching_) {
            start_fetch();
        }
    }

    void notify_waiters() noexcept {
        notify_timer_.reset();
        // Swapped out first: a waiter that verifies again may start the next fetch and wait
        // on it, and cancel_wait() may clear a slot not yet reached.
        notifying_.swap(waiters_);
        for (IKeyWaiter*& slot : notifying_) {
            if (IKeyWaiter* waiter = std::exchange(slot, nullptr)) {
                waiter->on_keys_refreshed();
            }
        }
        notifying_.clear();
    }

    void cancel(std::optional<net::TimerId>& timer) noexcept {
        if (timer) {
            reactor_.cancel_timer(*timer);
            timer.reset();
        }
    }

    template <void (State::*Fire)() noexcept> class Timer final : public net::ITimerHandler {
    public:
        explicit Timer(State& state) noexcept : state_(state) {}
        void on_timeout() noexcept override { (state_.*Fire)(); }

    private:
        State& state_;
    };

    net::IReactor& reactor_;
    IKeySetFetcher& fetcher_;
    const JwksConfig config_;

    detail::KeySet keys_;
    detail::ResultCache verdicts_;
    bool fetching_ = false;
    std::uint32_t failed_fetches_ = 0;
    std::optional<core::MonoTime> last_fetch_end_;
    // The end of the last fetch that brought keys, and whether they have since been dropped.
    std::optional<core::MonoTime> fetched_at_;
    bool expired_ = false;

    std::vector<IKeyWaiter*> waiters_;
    std::vector<IKeyWaiter*> notifying_;
    // Unseen kids waiting on the fetch in flight; those it does not bring are remembered.
    std::vector<std::string> sought_kids_;
    std::deque<UnknownKid> unknown_kids_;

    Timer<&State::on_refetch_due> refetch_due_{*this};
    Timer<&State::notify_waiters> notify_due_{*this};
    std::optional<net::TimerId> refetch_timer_;
    std::optional<net::TimerId> notify_timer_;
};

JwksVerifier::JwksVerifier(net::IReactor& reactor, IKeySetFetcher& fetcher, JwksConfig config)
    : state_(std::make_unique<State>(reactor, fetcher, std::move(config))) {}

JwksVerifier::~JwksVerifier() = default;

std::optional<VerifyResult> JwksVerifier::verify(std::string_view token, core::WallTime now,
                                                 IKeyWaiter& waiter) {
    return state_->verify(token, now, waiter);
}

void JwksVerifier::cancel_wait(IKeyWaiter& waiter) noexcept {
    state_->cancel_wait(waiter);
}

bool JwksVerifier::keys_expired() const noexcept {
    return state_->keys_expired();
}

} // namespace infra::auth
