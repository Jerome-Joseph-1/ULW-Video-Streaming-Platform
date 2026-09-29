#pragma once

#include "core/models/ids.hpp"
#include "core/util/time.hpp"

#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace core::ports {

enum class MediaError : std::uint8_t {
    // The media server did not answer, or answered that it is overloaded; a retry may succeed.
    Unavailable,
    // The media server refused the request as made: bad credentials, a bad argument. A retry
    // cannot succeed until the configuration changes.
    Refused,
    // The room was closed through this handle; its generation admits nobody again.
    Closed,
};

[[nodiscard]] std::string_view to_string(MediaError e) noexcept;

// What one participant's client needs to reach the media server itself (ADR-0050): the
// endpoint to connect to and a credential that admits exactly that participant to exactly
// that room until `expires_at`. The client runs WebRTC, offer and answer included, against the
// media server; the credential is the only thing the signalling path hands it.
struct MediaTicket {
    std::string endpoint;
    std::string credential;
    WallTime expires_at;
};

using MediaDone = std::move_only_function<void(std::expected<void, MediaError>) noexcept>;
using TicketDone = std::move_only_function<void(std::expected<MediaTicket, MediaError>) noexcept>;

// A call's media runs in one media-server room per generation, and a generation, once closed,
// never opens again. That is how a participant is put out: the media server keeps a connected
// client's credential fresh for as long as it stays connected, so no ticket can be withdrawn,
// but a closed generation admits nobody. The caller owns the number: it lives in the room's
// state, moves only forward, and moves only through the owner's fenced write (ADR-0015), so a
// deposed owner can neither open a generation nor close the current one (ADR-0050).
enum class MediaGeneration : std::uint64_t {};

enum class MediaRole : std::uint8_t {
    // A call member: publishes camera and microphone and receives everyone else. Its ticket's
    // endpoint is where the client's SDK connects.
    Member,
    // A live stream's source (M30): publishes camera and microphone, receives nothing. Its
    // ticket's endpoint takes a WHIP offer (RFC 9725) with the credential as bearer token, so
    // an encoder or a browser can publish with no SDK.
    Publisher,
};

class IMediaRoom {
public:
    virtual ~IMediaRoom() = default;
    // The device keeps two devices of one user apart: each is its own participant. The media
    // server drops a room that has stood empty for a while, and a handle can outlive that, so
    // join opens the generation again first (idempotent) and never issues a ticket for a room
    // that is gone.
    virtual void join(const UserId& user, const DeviceId& device, MediaRole role,
                      TicketDone done) = 0;
    // Ends this generation for everyone in it; their tickets and refreshed credentials stop
    // admitting anyone. Closing a generation the media server has already dropped succeeds.
    virtual void close(MediaDone done) = 0;
};

// Every member runs on the reactor thread, and every callback runs there later, never from
// inside the call that was given it. Rooms must not outlive their ISfu; destroying the ISfu
// drops the callbacks still pending.
class ISfu {
public:
    using OpenDone = std::move_only_function<void(
        std::expected<std::unique_ptr<IMediaRoom>, MediaError>) noexcept>;

    virtual ~ISfu() = default;
    // Idempotent for one generation. `max_participants` 0 means no limit of the room's own.
    virtual void open_room(const RoomId& room, MediaGeneration generation,
                           std::uint16_t max_participants, OpenDone done) = 0;
};

} // namespace core::ports
