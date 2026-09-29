#pragma once

#include "infra/auth/jwks_verifier.hpp"
#include "infra/curl/fetcher.hpp"

#include "ops/log.hpp"

#include <cstdint>
#include <vector>

namespace gateway {

// Fetches the verifier's key set over HTTPS on a libcurl multi driven by the reactor.
class KeySetFetcher final : public infra::auth::IKeySetFetcher {
public:
    KeySetFetcher(infra::curl::Multi& multi, ops::Logger& log) noexcept;

    void fetch(std::string_view url, infra::auth::IKeySetReceiver& receiver) noexcept override;
    void cancel(infra::auth::IKeySetReceiver& receiver) noexcept override;

    // Transfers still running, cancelled ones included.
    [[nodiscard]] std::size_t in_flight() const noexcept { return http_.pending(); }

private:
    struct Waiting {
        infra::auth::IKeySetReceiver* receiver;
        // Tells a cancelled fetch's late result apart from a newer fetch by the same receiver.
        std::uint64_t fetch;
    };

    void finished(std::uint64_t fetch, infra::curl::Result result) noexcept;

    infra::curl::HttpFetcher http_;
    ops::Logger& log_;
    std::vector<Waiting> waiting_;
    std::uint64_t next_fetch_ = 0;
};

} // namespace gateway
