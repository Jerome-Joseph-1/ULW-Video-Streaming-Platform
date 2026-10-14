#pragma once

#include "infra/turn/turn.hpp"

#include <memory>
#include <vector>

namespace infra::turn {

// Asks Cloudflare's API for each client's ICE servers (generate-ice-servers), one request per
// ticket; the credentials it returns are its own, revocable by username.
class CloudflareCredentials final : public core::ports::ITurnCredentials {
public:
    CloudflareCredentials(net::IReactor& reactor, curl::Multi& multi, Config config) noexcept;
    ~CloudflareCredentials() override;
    CloudflareCredentials(const CloudflareCredentials&) = delete;
    CloudflareCredentials& operator=(const CloudflareCredentials&) = delete;

    void issue(const core::UserId& user, const core::DeviceId& device,
               core::ports::IceServersDone done) override;

private:
    class Request;

    void finished(Request& request,
                  std::expected<std::vector<core::ports::IceServer>, core::ports::MediaError>
                      outcome) noexcept;

    net::IReactor& reactor_;
    curl::Multi& multi_;
    Config config_;
    std::string url_;
    std::vector<std::unique_ptr<Request>> requests_;
};

} // namespace infra::turn
