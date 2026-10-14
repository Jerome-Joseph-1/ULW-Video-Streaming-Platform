#pragma once

#include "infra/turn/turn.hpp"

#include <memory>
#include <vector>

namespace infra::turn {

// Mints each credential itself from the shared secret; nothing crosses the network.
class HmacCredentials final : public core::ports::ITurnCredentials {
public:
    HmacCredentials(net::IReactor& reactor, const core::ports::IClock& clock,
                    Config config) noexcept;
    ~HmacCredentials() override;
    HmacCredentials(const HmacCredentials&) = delete;
    HmacCredentials& operator=(const HmacCredentials&) = delete;

    void issue(const core::UserId& user, const core::DeviceId& device,
               core::ports::IceServersDone done) override;

private:
    struct Pending final : net::ITimerHandler {
        explicit Pending(HmacCredentials& o) noexcept : owner(o) {}
        void on_timeout() noexcept override;

        HmacCredentials& owner;
        std::vector<core::ports::IceServer> servers;
        core::ports::IceServersDone done;
        net::TimerId timer{};
    };

    void deliver(Pending& pending) noexcept;

    net::IReactor& reactor_;
    const core::ports::IClock& clock_;
    Config config_;
    std::vector<std::unique_ptr<Pending>> pending_;
};

} // namespace infra::turn
