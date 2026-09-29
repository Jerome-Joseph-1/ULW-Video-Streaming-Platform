#pragma once

#include "core/ports/auth.hpp"

#include <string_view>
#include <vector>

namespace ulw::test {

// "user.<sub>" verifies as <sub>; anything else is a bad signature. "slow.<sub>" behaves like
// a token whose key is not cached yet: the first attempt waits for refresh_keys(). "down.<sub>"
// fails as if the key server were unreachable. Dots, not
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
        return core::ports::Claims{
            .subject = *user, .email = {}, .expires_at = now + std::chrono::hours(1)};
    }

    void cancel_wait(core::ports::IKeyWaiter& waiter) noexcept override {
        std::erase(waiters_, &waiter);
    }

    [[nodiscard]] std::size_t waiting() const noexcept { return waiters_.size(); }

    void refresh_keys() {
        refreshed_ = true;
        auto waiters = std::move(waiters_);
        for (auto* w : waiters) {
            w->on_keys_refreshed();
        }
    }

private:
    bool refreshed_ = false;
    std::vector<core::ports::IKeyWaiter*> waiters_;
};

} // namespace ulw::test
