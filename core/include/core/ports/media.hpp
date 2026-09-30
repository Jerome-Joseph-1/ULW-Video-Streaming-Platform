#pragma once

#include "core/models/ids.hpp"
#include "core/util/time.hpp"

#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace core::ports {

enum class MediaError : std::uint8_t {
    // The media server did not answer, or answered that it is overloaded; a retry may succeed.
    Unavailable,
    // The media server refused the request as made: bad credentials, a bad argument. A retry
    // cannot succeed until the configuration changes.
    Refused,
    // The room was closed through this handle; its generation admits nobody again.
    Closed,
    // The media server supports the call, but this adapter does not implement it yet (group
    // calls, ADR-0058). Permanent for the build: retrying cannot help.
    NotImplemented,
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

// One connected device in a room, as the media server reports it. Group calls (M27) read the
// roster to check that a room holds everyone who was admitted and no one else.
struct MediaParticipant {
    UserId user;
    DeviceId device;
    WallTime joined_at;
};

using ParticipantsDone = std::move_only_function<void(
    std::expected<std::vector<MediaParticipant>, MediaError>) noexcept>;
using MediaDone = std::move_only_function<void(std::expected<void, MediaError>) noexcept>;
using TicketDone = std::move_only_function<void(std::expected<MediaTicket, MediaError>) noexcept>;

// A call's media runs in one media-server room per generation, and a generation, once closed,
// is never opened again by the caller. That is how a participant is put out: the media server
// keeps a connected client's credential fresh for as long as it stays connected, so no ticket
// can be withdrawn, but a closed generation admits no member. A publisher ticket is the
// exception: until it expires, a WHIP POST with it brings its closed generation's room back
// (the media server skips its own room check on that path), which is why it lives no longer
// than a member's ticket (ADR-0053). The caller owns the number: it lives in the room's state,
// moves only forward, and moves only through the owner's fenced write (ADR-0015), so a deposed
// owner can neither open a generation nor close the current one (ADR-0050).
enum class MediaGeneration : std::uint64_t {};

enum class MediaRole : std::uint8_t {
    // A call member: publishes camera and microphone and receives everyone else. Its ticket's
    // endpoint is where the client's SDK connects.
    Member,
    // A live stream's source (M30): publishes camera and microphone, receives nothing. Its
    // ticket's endpoint takes a WHIP offer (RFC 9725) with the credential as bearer token, so
    // an encoder or a browser can publish with no SDK. It lives as long as a member's ticket;
    // WHIP sends a bearer token with every later request on the session too, so a client asks
    // for a fresh ticket for each PATCH and for its DELETE (ADR-0053).
    Publisher,
};

// What a room carries, fixed per handle by the caller that opened it: a call's room issues
// member tickets only, a stream's room publisher tickets only, and only a stream's room may be
// relayed to a packager.
enum class MediaRoomKind : std::uint8_t {
    Call,
    Stream,
};

// A live stream's packager, as the relay reaches it (ADR-0046, ADR-0053): which stream it takes,
// the secret it admits the relay with, and the segment length it cuts at, which becomes the
// relay's keyframe interval. Where the packager listens is the media adapter's configuration,
// not the caller's to choose.
struct MediaRelay {
    std::string stream;
    std::string passphrase;
    Seconds keyframe_interval;
};

// The relay's id at the media server.
using RelayDone = std::move_only_function<void(std::expected<std::string, MediaError>) noexcept>;

class IMediaRoom {
public:
    virtual ~IMediaRoom() = default;
    // The device keeps two devices of one user apart: each is its own participant. The media
    // server drops a room that has stood empty for a while, and a handle can outlive that, so
    // join opens the generation again first (idempotent) and never issues a ticket for a room
    // that is gone.
    virtual void join(const UserId& user, const DeviceId& device, MediaRole role,
                      TicketDone done) = 0;
    // Who is connected to this generation right now, in no particular order; a ticket that has
    // been issued but not used does not count. Group calls only (ADR-0058): an adapter without
    // them reports `NotImplemented`.
    virtual void participants(ParticipantsDone done) = 0;
    // Sends what that participant publishes to `target`'s packager, re-encoded for it, until
    // the participant leaves or the generation closes; its leaving is how the packager learns
    // the stream is over. The participant must be in the room already: the media server looks
    // for it for half a minute, not for as long as a ticket lasts. Idempotent: calls made while
    // one for the same participant is in flight share its answer, and a participant already
    // relayed gets the running relay's id. One case is left: a retry after the start timed out
    // here, while the media server may still be starting it, can start a second (ADR-0053).
    // Refused for a call's room.
    virtual void relay(const UserId& user, const DeviceId& device, const MediaRelay& target,
                       RelayDone done) = 0;
    // Ends this generation for everyone in it; members' tickets and refreshed credentials stop
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
    virtual void open_room(const RoomId& room, MediaGeneration generation, MediaRoomKind kind,
                           std::uint16_t max_participants, OpenDone done) = 0;
};

} // namespace core::ports
