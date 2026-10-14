#pragma once

#include "core/models/ids.hpp"
#include "core/ports/media.hpp"

#include <expected>
#include <functional>
#include <vector>

namespace core::ports {

using IceServersDone =
    std::move_only_function<void(std::expected<std::vector<IceServer>, MediaError>) noexcept>;

// Credentials for a TURN service outside the cluster (ADR-0109): the relays one participant's
// client reaches the media server through, each with a credential that admits that client
// alone for a while. `Unavailable` when the service did not answer, `Refused` when it refused
// our own credentials, so that only a configuration change helps.
//
// Runs on the reactor thread, and `done` runs there later, never from inside `issue`.
// Destroying the issuer drops the callbacks still pending.
class ITurnCredentials {
public:
    virtual ~ITurnCredentials() = default;
    virtual void issue(const UserId& user, const DeviceId& device, IceServersDone done) = 0;
};

} // namespace core::ports
