#pragma once

#include "codec/sdp/error.hpp"
#include "codec/sdp/session.hpp"

#include <optional>
#include <string_view>

namespace codec::sdp::detail {

// RTP/AVP, RTP/SAVPF, UDP/TLS/RTP/SAVPF and the like: formats are payload types.
[[nodiscard]] bool carries_rtp(std::string_view protocol) noexcept;

// What the signalling boundary enforces on a description that parsed.
[[nodiscard]] std::optional<Error> validate(const Session& session);

} // namespace codec::sdp::detail
