#pragma once

#include "core/ports/auth.hpp"
#include "core/util/time.hpp"
#include "infra/auth/claim_rules.hpp"
#include "net/reactor.hpp"

#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace infra::auth {

class IKeySetReceiver {
public:
    virtual ~IKeySetReceiver() = default;
    // On the reactor thread. `body` is the body of a 200 response and is valid only during
    // the call; nullopt stands for any failure, all of which the verifier treats alike.
    virtual void on_key_set(std::optional<std::string_view> body) noexcept = 0;
};

// Fetches a JWK set over HTTPS; infra/curl implements it over libcurl.
class IKeySetFetcher {
public:
    virtual ~IKeySetFetcher() = default;
    // One fetch per receiver at a time. The receiver hears back exactly once, unless
    // cancel() comes first.
    virtual void fetch(std::string_view url, IKeySetReceiver& receiver) noexcept = 0;
    // No callback follows. A no-op when nothing is in flight for `receiver`.
    virtual void cancel(IKeySetReceiver& receiver) noexcept = 0;
};

struct JwksConfig {
    // JWKS_URL
    std::string url;
    ClaimRules claims;
};

// Verifies tokens against the JWK set at a URL, fetched on the reactor thread and cached:
//  - The keys are refetched every 15 minutes in the background. They stay in use until a
//    refetch succeeds, and a failed refetch is retried with backoff.
//  - A token with an unseen kid waits for one fetch, shared by every token that arrives
//    while it runs. A kid still missing afterwards is refused and remembered for a minute,
//    and fetches for unseen kids are 10 s apart at the least, so junk kids cannot drive
//    fetches.
//  - A verified token is remembered by its digest for 15 minutes, never past its expiry.
// Every member must be called on the reactor thread.
class JwksVerifier final : public core::ports::IJwtVerifier {
public:
    JwksVerifier(net::IReactor& reactor, IKeySetFetcher& fetcher, JwksConfig config);
    // Cancels the fetch in flight and the timers; waiters still pending are not called.
    ~JwksVerifier() override;
    JwksVerifier(const JwksVerifier&) = delete;
    JwksVerifier& operator=(const JwksVerifier&) = delete;
    JwksVerifier(JwksVerifier&&) = delete;
    JwksVerifier& operator=(JwksVerifier&&) = delete;

    [[nodiscard]] std::optional<core::ports::VerifyResult>
    verify(std::string_view token, core::WallTime now, core::ports::IKeyWaiter& waiter) override;
    void cancel_wait(core::ports::IKeyWaiter& waiter) noexcept override;

private:
    class State;
    std::unique_ptr<State> state_;
};

} // namespace infra::auth
