#pragma once

#include "core/models/ids.hpp"

#include "wire.hpp"

#include <optional>
#include <span>

// The node channel's handshake (ADR-0037). Both ends hold the same secret. The dialer sends a
// fresh nonce, the owner answers with its own and a tag over both, the dialer answers with its
// tag over both: each proves it holds the secret over a nonce the other just chose, so a
// recorded exchange replays to nothing. The tags differ by role, so one end's tag is never
// the other's, and name both nodes, so a tag for one pair never passes for another.
namespace rt::auth {

// nullopt only if the crypto library fails.
[[nodiscard]] std::optional<wire::Mac> acceptor_tag(std::span<const std::byte> secret,
                                                    const core::NodeId& dialer,
                                                    const core::NodeId& acceptor,
                                                    const wire::Nonce& dialer_nonce,
                                                    const wire::Nonce& acceptor_nonce);
[[nodiscard]] std::optional<wire::Mac> dialer_tag(std::span<const std::byte> secret,
                                                  const core::NodeId& dialer,
                                                  const core::NodeId& acceptor,
                                                  const wire::Nonce& dialer_nonce,
                                                  const wire::Nonce& acceptor_nonce);

// In constant time, so the comparison says nothing about how much of a forged tag was right.
[[nodiscard]] bool same_tag(const wire::Mac& a, const wire::Mac& b) noexcept;

} // namespace rt::auth
