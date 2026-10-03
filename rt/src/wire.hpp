#pragma once

#include "core/models/ids.hpp"
#include "core/ports/message_store.hpp"
#include "rt/message_key.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <variant>
#include <vector>

// The node channel's frames (ADR-0035): a 4-byte big-endian length, then a type byte and the
// type's fields. Integers are big-endian, a room is its 36-character canonical text, and a
// node or sender is a length byte and its characters. A body runs to the end of its frame.
namespace rt::wire {

// 2: Send and Deliver carry the client's message key. 3: a Reply may say Conflict. 4: Ask and
// Answer carry what a room's owner answers for its members (a call's ticket, ADR-0050). 5: Notify
// and Notice carry what any node hands a room's members, unsequenced (a call's ring, ADR-0092).
inline constexpr std::uint8_t kVersion = 5;
// The largest message a client may send (codec::ws::Decoder's 64 KiB, ADR-0029), plus room for
// the other fields, which take at most 239 bytes (a Send: type, request, room, sender, key).
inline constexpr std::size_t kMaxBody = std::size_t{64} * 1024;
inline constexpr std::size_t kMaxFrame = kMaxBody + 256;
// The store takes any body the node channel carries (ADR-0054).
static_assert(kMaxBody == core::ports::kMaxMessageBody);
// Handshake nonces, and the HMAC-SHA256 tags over them.
inline constexpr std::size_t kNonceBytes = 32;
inline constexpr std::size_t kMacBytes = 32;

using Nonce = std::array<std::byte, kNonceBytes>;
using Mac = std::array<std::byte, kMacBytes>;

enum class Type : std::uint8_t {
    // Dialer to owner, once, first: the dialer's version, node and nonce.
    Hello = 1,
    // Dialer to owner: deliver the room's messages to me. Answered with a Reply.
    Subscribe = 2,
    // Dialer to owner: stop delivering the room's messages. Owner to dialer: this node no
    // longer owns the room; look up its new owner. Not answered either way.
    Unsubscribe = 3,
    // Dialer to owner: sequence this message. Answered with a Reply.
    Send = 4,
    // Owner to dialer: the outcome of a Subscribe or Send.
    Reply = 5,
    // Owner to dialer: a sequenced message of a room the dialer subscribed to.
    Deliver = 6,
    // Owner to dialer, answering Hello: the owner's node, its nonce, and its proof that it
    // holds the channel's secret.
    Challenge = 7,
    // Dialer to owner, answering Challenge: the dialer's proof. Nothing else is taken first.
    Proof = 8,
    // Dialer to owner: a request the room's owner answers itself (IOwnerService). Answered with
    // an Answer.
    Ask = 9,
    // Owner to dialer: the outcome of an Ask, and the owner's answer when it is Ok.
    Answer = 10,
    // Any node to the room's owner: hand this to the room's members wherever they are. Not
    // answered; an owner that does not hold the room drops it.
    Notify = 11,
    // Owner to dialer: a Notify for a room the dialer subscribed to, for its members there.
    Notice = 12,
};

enum class Status : std::uint8_t {
    Ok = 0,
    // The node does not own the room; the dialer's idea of the owner is out of date.
    NotOwner = 1,
    // The owner's write was fenced: the room changed hands under it. Nothing was written.
    Fenced = 2,
    // The owner could not reach the store; whether the write happened is unknown.
    Unavailable = 3,
    // The room's queue of writes is full.
    Busy = 4,
    // The sender's key is stored with another body. Nothing was written.
    Conflict = 5,
};

struct Hello {
    std::uint8_t version = 0;
    core::NodeId node;
    Nonce nonce{};
};
struct Challenge {
    core::NodeId node;
    Nonce nonce{};
    Mac mac{};
};
struct Proof {
    Mac mac{};
};
struct Subscribe {
    std::uint64_t request = 0;
    core::RoomId room;
};
struct Unsubscribe {
    core::RoomId room;
};
// The body views the decoder's buffer and stays valid until the next feed() or next().
struct Send {
    std::uint64_t request = 0;
    core::RoomId room;
    core::UserId sender;
    MessageKey key;
    std::span<const std::byte> body;
};
struct Reply {
    std::uint64_t request = 0;
    Status status = Status::Ok;
    std::uint64_t seq = 0;
};
// As Send, the body is a view.
struct Deliver {
    core::RoomId room;
    std::uint64_t seq = 0;
    core::UserId sender;
    MessageKey key;
    std::span<const std::byte> body;
};

// The body is a view, as Send's; at most kMaxBody either way.
struct Ask {
    std::uint64_t request = 0;
    core::RoomId room;
    std::span<const std::byte> body;
};
struct Answer {
    std::uint64_t request = 0;
    Status status = Status::Ok;
    std::span<const std::byte> body;
};

// Notify and Notice: a room and a body, the body a view as Send's; at most kMaxBody.
struct Notify {
    core::RoomId room;
    std::span<const std::byte> body;
};
struct Notice {
    core::RoomId room;
    std::span<const std::byte> body;
};

using Frame = std::variant<Hello, Challenge, Proof, Subscribe, Unsubscribe, Send, Reply, Deliver,
                           Ask, Answer, Notify, Notice>;

void encode_hello(std::vector<std::byte>& out, const core::NodeId& node, const Nonce& nonce);
void encode_challenge(std::vector<std::byte>& out, const core::NodeId& node, const Nonce& nonce,
                      const Mac& mac);
void encode_proof(std::vector<std::byte>& out, const Mac& mac);
void encode_subscribe(std::vector<std::byte>& out, std::uint64_t request, const core::RoomId& room);
void encode_unsubscribe(std::vector<std::byte>& out, const core::RoomId& room);
// A body above kMaxBody is the caller's bug; the client decoder never produces one.
void encode_send(std::vector<std::byte>& out, std::uint64_t request, const core::RoomId& room,
                 const core::UserId& sender, const MessageKey& key,
                 std::span<const std::byte> body);
void encode_reply(std::vector<std::byte>& out, std::uint64_t request, Status status,
                  std::uint64_t seq);
void encode_deliver(std::vector<std::byte>& out, const core::RoomId& room, std::uint64_t seq,
                    const core::UserId& sender, const MessageKey& key,
                    std::span<const std::byte> body);
// Bodies above kMaxBody are the caller's bug, as for encode_send.
void encode_ask(std::vector<std::byte>& out, std::uint64_t request, const core::RoomId& room,
                std::span<const std::byte> body);
void encode_answer(std::vector<std::byte>& out, std::uint64_t request, Status status,
                   std::span<const std::byte> body);
// Bodies above kMaxBody are the caller's bug, as for encode_send.
void encode_notify(std::vector<std::byte>& out, const core::RoomId& room,
                   std::span<const std::byte> body);
void encode_notice(std::vector<std::byte>& out, const core::RoomId& room,
                   std::span<const std::byte> body);

enum class DecodeError : std::uint8_t {
    // A length of zero or above kMaxFrame.
    BadLength,
    UnknownType,
    // Fields missing, left over, or invalid (a room, node, sender or key that does not parse).
    Malformed,
};

// Holds at most one partial frame plus what the last feed() brought.
class Decoder {
public:
    void feed(std::span<const std::byte> bytes);
    // The next whole frame, nullopt when none is complete. After an error the stream is lost.
    [[nodiscard]] std::expected<std::optional<Frame>, DecodeError> next();

private:
    std::vector<std::byte> buffer_;
    std::size_t consumed_ = 0;
    bool failed_ = false;
};

} // namespace rt::wire
