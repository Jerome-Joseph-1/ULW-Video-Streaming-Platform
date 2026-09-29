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
};

[[nodiscard]] std::string_view to_string(MediaError e) noexcept;

// What one participant's client needs to reach the media server itself (ADR-0033): the
// endpoint to connect to and a credential that admits exactly that participant to exactly
// that room until `expires_at`. The client runs WebRTC, offer and answer included, against the
// media server; the credential is the only thing the signalling path hands it.
struct MediaTicket {
    std::string endpoint;
    std::string credential;
    WallTime expires_at;
};

using MediaDone = std::move_only_function<void(std::expected<void, MediaError>) noexcept>;

class IMediaParticipant {
public:
    virtual ~IMediaParticipant() = default;
    [[nodiscard]] virtual const MediaTicket& ticket() const noexcept = 0;
    // Takes the participant out of the room, ending its media for everyone. Done when the media
    // server has confirmed; a participant that never connected, or already left, counts as
    // removed.
    virtual void remove(MediaDone done) = 0;
};

class IMediaRoom {
public:
    virtual ~IMediaRoom() = default;
    // The device keeps two devices of one user apart: each is its own participant.
    [[nodiscard]] virtual std::expected<std::unique_ptr<IMediaParticipant>, MediaError>
    join(const UserId& user, const DeviceId& device) = 0;
    // Ends the room for everyone in it. Closing a room the media server has already dropped
    // succeeds.
    virtual void close(MediaDone done) = 0;
};

// Every member runs on the reactor thread, and every callback runs there later, never from
// inside the call that was given it. Rooms and participants must not outlive their ISfu;
// destroying the ISfu drops the callbacks still pending.
class ISfu {
public:
    using OpenDone = std::move_only_function<void(
        std::expected<std::unique_ptr<IMediaRoom>, MediaError>) noexcept>;

    virtual ~ISfu() = default;
    // Idempotent: opening a room that is already open yields the same room.
    virtual void open_room(const RoomId& room, std::uint16_t max_participants, OpenDone done) = 0;
};

} // namespace core::ports
