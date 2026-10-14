#include "hmac.hpp"

#include "infra/turn/turn.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <optional>
#include <string>
#include <utility>

namespace infra::turn {

namespace detail {

core::ports::IceServer hmac_server(const std::vector<std::string>& urls, std::string_view secret,
                                   std::string_view name, std::int64_t expiry_unix) {
    std::string username = std::to_string(expiry_unix);
    username += ':';
    username += name;
    std::array<unsigned char, EVP_MAX_MD_SIZE> mac{};
    unsigned int mac_size = 0;
    // SHA-1 because the credential format is HMAC-SHA1: it is what coturn, eturnal and LiveKit
    // check (RFC 5389's long-term mechanism and the TURN REST API draft). An HMAC key-derivation
    // use, not a collision-sensitive one; the credential's strength is the secret's.
    HMAC(EVP_sha1(), secret.data(), static_cast<int>(secret.size()), // NOSONAR(cpp:S4790)
         reinterpret_cast<const unsigned char*>(username.data()), username.size(), mac.data(),
         &mac_size);
    // 4 output bytes for every 3 input bytes, rounded up, and the terminator EVP writes.
    std::array<unsigned char, ((EVP_MAX_MD_SIZE + 2) / 3 * 4) + 1> text{};
    const int length = EVP_EncodeBlock(text.data(), mac.data(), static_cast<int>(mac_size));
    return core::ports::IceServer{.urls = urls,
                                  .username = std::move(username),
                                  .credential =
                                      std::string(reinterpret_cast<const char*>(text.data()),
                                                  static_cast<std::size_t>(std::max(length, 0)))};
}

} // namespace detail

HmacCredentials::HmacCredentials(net::IReactor& reactor, const core::ports::IClock& clock,
                                 Config config) noexcept
    : reactor_(reactor), clock_(clock), config_(std::move(config)) {}

HmacCredentials::~HmacCredentials() {
    for (const auto& pending : pending_) {
        reactor_.cancel_timer(pending->timer);
    }
}

void HmacCredentials::issue(const core::UserId& /*user*/, const core::DeviceId& device,
                            core::ports::IceServersDone done) {
    // The device rather than the user names the credential: the TURN service logs usernames,
    // and a device id is a random UUID that says nothing about whose it is.
    const auto expiry = std::chrono::duration_cast<core::Seconds>(
                            (clock_.wall_now() + config_.ttl).time_since_epoch())
                            .count();
    auto& pending = *pending_.emplace_back(std::make_unique<Pending>(*this));
    pending.servers = {
        detail::hmac_server(config_.urls, config_.shared_secret, device.to_string(), expiry)};
    pending.done = std::move(done);
    // Computed here, delivered later: the port never calls back from inside issue.
    pending.timer = reactor_.arm_timer(core::Millis{0}, pending);
}

void HmacCredentials::deliver(Pending& pending) noexcept {
    auto done = std::move(pending.done);
    auto servers = std::move(pending.servers);
    std::erase_if(pending_, [&](const auto& p) { return p.get() == &pending; });
    // Last: the callback may destroy this issuer.
    done(std::move(servers));
}

void HmacCredentials::Pending::on_timeout() noexcept {
    owner.deliver(*this);
}

} // namespace infra::turn
