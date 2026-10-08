#pragma once

#include "core/ports/auth.hpp"

#include <string_view>
#include <utility>
#include <vector>

namespace ulw::test {

// "user.<sub>" verifies as <sub>; anything else is a bad signature. "slow.<sub>" behaves like
// a token whose key is not cached yet: the first attempt waits for refresh_keys(). "down.<sub>"
// fails as if the key server were unreachable. "forever.<sub>" expires at the latest instant a
// token's exp can name (the wall clock's last whole second but one). "viewer.<sub>" verifies
// but does not carry what the deployment asks of a broadcaster. "service.<sub>" is the
// operator's backend (ULW_SERVICE_SCOPE, ADR-0096). "watcher.<sub>" verifies but does not carry
// what the deployment asks of an uploader (ULW_UPLOADER_SCOPE, ADR-0100). Dots, not
// colons, so the tokens pass the gateway's check that a token looks like a compact JWS.
class FakeVerifier final : public core::ports::IJwtVerifier {
public:
    std::optional<core::ports::VerifyResult> verify(std::string_view token, core::WallTime now,
                                                    core::ports::IKeyWaiter& waiter) override {
        if (token.starts_with("down.")) {
            return std::unexpected(core::ports::AuthError::KeysUnavailable);
        }
        if (token.starts_with("slow.") && !refreshed_) {
            waiters_.push_back(&waiter);
            return std::nullopt;
        }
        const std::size_t dot = token.find('.');
        if (dot == std::string_view::npos) {
            return std::unexpected(core::ports::AuthError::BadSignature);
        }
        auto user = core::UserId::parse(token.substr(dot + 1));
        if (!user) {
            return std::unexpected(core::ports::AuthError::MissingSubject);
        }
        if (token.starts_with("forever.")) {
            constexpr auto kLast =
                std::chrono::floor<std::chrono::seconds>(core::WallTime::duration::max()) -
                std::chrono::seconds(1);
            return core::ports::Claims{
                .subject = *user, .email = {}, .expires_at = core::WallTime{kLast}};
        }
        return core::ports::Claims{.subject = *user,
                                   .email = {},
                                   .expires_at = now + std::chrono::hours(1),
                                   .may_broadcast = !token.starts_with("viewer."),
                                   .is_service = token.starts_with("service."),
                                   .may_upload = !token.starts_with("watcher.")};
    }

    void cancel_wait(core::ports::IKeyWaiter& waiter) noexcept override {
        std::erase(waiters_, &waiter);
    }

    [[nodiscard]] std::size_t waiting() const noexcept { return waiters_.size(); }

    // What keys_expired() reports; verify() is unchanged by it.
    bool expired = false;
    [[nodiscard]] bool keys_expired() const noexcept override { return expired; }

    // Calls to drop_caches(), which changes nothing else here: what a drop does to tokens is
    // infra/auth's to test, with keys.
    std::size_t drops = 0;
    void drop_caches() noexcept override { ++drops; }
    // What drop_pending() reports.
    bool drop_requested = false;
    [[nodiscard]] bool drop_pending() const noexcept override { return drop_requested; }

    void refresh_keys() {
        refreshed_ = true;
        // Taken out first, leaving the member empty and usable: a waiter that registers again
        // from its callback waits for the next refresh.
        const auto pending = std::exchange(waiters_, {});
        for (auto* w : pending) {
            w->on_keys_refreshed();
        }
    }

private:
    bool refreshed_ = false;
    std::vector<core::ports::IKeyWaiter*> waiters_;
};

} // namespace ulw::test
