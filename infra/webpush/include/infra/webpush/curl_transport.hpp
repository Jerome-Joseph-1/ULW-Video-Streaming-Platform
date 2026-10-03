#pragma once

#include "infra/curl/multi.hpp"
#include "infra/webpush/sender.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

namespace infra::webpush {

struct CurlTransportOptions {
    // The whole exchange: a push service answers in well under a second.
    std::chrono::milliseconds timeout{10'000};
    // Connect only to global unicast addresses, never through a proxy (Request::public_only).
    // Off only for a test whose push service is on loopback (ULW_DEV_PUSH_ALLOW_PRIVATE).
    bool public_only = true;
    // A test CA to trust instead of the system's (ULW_DEV_PUSH_CA_FILE); empty in production.
    // NOLINTNEXTLINE(readability-redundant-member-init)
    std::string ca_file = {};
};

// IPushTransport over a libcurl multi on the reactor: https only, TLS 1.3, no redirects
// (infra::curl's defaults), the body streamed from memory.
class CurlPushTransport final : public IPushTransport {
public:
    CurlPushTransport(curl::Multi& multi, CurlTransportOptions options) noexcept;
    ~CurlPushTransport() override;
    CurlPushTransport(const CurlPushTransport&) = delete;
    CurlPushTransport& operator=(const CurlPushTransport&) = delete;

    [[nodiscard]] bool post(PushRequest request, Done done) noexcept override;
    [[nodiscard]] std::size_t running() const noexcept { return posts_.size(); }

private:
    class Post;
    void finished(std::uint64_t id) noexcept;

    curl::Multi& multi_;
    CurlTransportOptions options_;
    std::uint64_t next_ = 1;
    std::unordered_map<std::uint64_t, std::unique_ptr<Post>> posts_;
};

// The response, as the sender reads it.
[[nodiscard]] PushOutcome outcome_of(const curl::Result& result) noexcept;

} // namespace infra::webpush
