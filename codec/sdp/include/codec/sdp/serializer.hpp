#pragma once

#include "codec/sdp/session.hpp"

#include <string>

namespace codec::sdp {

// Lines in RFC 8866 order, each ended by CRLF. The Session is written as it is: validating it
// is the job of whoever built it, or of parse() on the result.
[[nodiscard]] std::string serialize(const Session& session);

} // namespace codec::sdp
