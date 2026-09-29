#pragma once

#include "core/ports/clock.hpp"
#include "core/ports/media.hpp"
#include "infra/curl/multi.hpp"
#include "net/reactor.hpp"

#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <string_view>

namespace infra::sfu::livekit {

struct Config {
    // Where RoomService answers, "http://livekit:7880"; the server's own calls go here.
    std::string api_url;
    // What browsers connect to, "wss://media.example.com"; it goes into every ticket.
    std::string client_url;
    std::string api_key;
    std::string api_secret;
};

enum class ConfigError : std::uint8_t {
    BadApiUrl,
    BadClientUrl,
    MissingApiKey,
    // Shorter than 32 bytes, or implausibly long.
    BadApiSecret,
};

[[nodiscard]] std::string_view to_string(ConfigError e) noexcept;

// Each generation of a room is the LiveKit room "<room id>:<generation>"; a participant is the
// LiveKit identity "<user>/<device>". Room control goes over RoomService (Twirp, JSON over HTTP) on
// `reactor` through `multi`; all three arguments must outlive the returned ISfu.
[[nodiscard]] std::expected<std::unique_ptr<core::ports::ISfu>, ConfigError>
make_sfu(net::IReactor& reactor, curl::Multi& multi, const core::ports::IClock& clock,
         Config config);

} // namespace infra::sfu::livekit
