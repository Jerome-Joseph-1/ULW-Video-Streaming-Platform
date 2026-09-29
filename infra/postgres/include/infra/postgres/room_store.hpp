#pragma once

#include "core/ports/message_store.hpp"
#include "net/offload_pool.hpp"
#include "net/reactor.hpp"
#include "rt/room_store.hpp"

#include <cstddef>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace infra::postgres {

struct RoomStoreConfig {
    std::string conninfo;
    // Every call is one statement on primary keys. A node sends a heartbeat and a sweep a
    // second plus one append per message; four sessions carry thousands of messages a second,
    // and three nodes hold 3 x (4 + 1 listening) = 15 of Postgres' default 100 connections.
    std::size_t connections = 4;
    // As the catalog's: a handful of round trips on the private network, or a server that is
    // gone.
    core::Millis connect_timeout{5000};
};

// IRoomStore on Postgres (migrations/0003_rooms.sql), driven by the reactor: no call blocks the
// loop, and each gives up after rt::kStoreTimeout. One more session LISTENs for ownership
// changes, and reports a resync whenever it (re)starts listening.
//
// The offload pool resolves host names and must be stopped before this is destroyed. Calls
// outstanding at destruction are dropped unanswered.
class PgRoomStore final : public rt::IRoomStore {
    class Impl;
    struct Token {
        explicit Token() = default;
    };

public:
    // Refuses a connection string that does not parse, and one that would make libpq look the
    // host up itself.
    [[nodiscard]] static std::expected<std::unique_ptr<PgRoomStore>, std::string>
    create(net::IReactor& reactor, net::OffloadPool& offload, const RoomStoreConfig& config);

    // Only create() can make the token.
    PgRoomStore(Token token, std::unique_ptr<Impl> impl) noexcept;
    ~PgRoomStore() override;
    PgRoomStore(const PgRoomStore&) = delete;
    PgRoomStore& operator=(const PgRoomStore&) = delete;
    PgRoomStore(PgRoomStore&&) = delete;
    PgRoomStore& operator=(PgRoomStore&&) = delete;

    void watch(rt::IOwnershipListener& listener) noexcept override;
    void resolve(const core::RoomId& room, const core::NodeId& node,
                 rt::StoreCallback<rt::Ownership> done) override;
    void claim_stale(std::vector<core::RoomId> rooms, const core::NodeId& node,
                     rt::StoreCallback<std::vector<rt::OwnedRoom>> done) override;
    void heartbeat(const core::NodeId& node, const core::Uuid& incarnation,
                   std::vector<rt::OwnedRoom> rooms,
                   rt::StoreCallback<std::vector<core::RoomId>> done) override;
    void append(const core::RoomId& room, std::uint64_t generation, const rt::Outgoing& message,
                rt::StoreCallback<std::optional<std::uint64_t>> done) override;
    // append(), and in the same statement the message's row in chat_messages
    // (migrations/0005_chat_messages.sql): the seq is taken only with its row, and a fenced
    // writer takes neither. Takes what the router's outgoing message carries (sender, key and
    // a body it holds only for the call), so that it is the router's append passed through;
    // sent_at is the database's clock at the write. A body over core::ports::kMaxMessageBody
    // or a key over kMaxMessageKey is TooLarge, and nothing is written.
    // Idempotent by the sender's `key`: a message whose key the sender already used in the room is
    // not stored again, and the answer is the seq it was stored under (still only to the room's
    // owner).
    void append_message(const core::RoomId& room, std::uint64_t generation,
                        const core::UserId& sender, std::string_view key,
                        std::span<const std::byte> body,
                        core::ports::MessageCallback<std::optional<std::uint64_t>> done);
    void release(const core::NodeId& node, std::vector<rt::OwnedRoom> rooms,
                 rt::StoreCallback<void> done) override;
    void advertise(const core::NodeId& node, std::string address, const core::Uuid& incarnation,
                   rt::StoreCallback<void> done) override;
    void read_owners(
        std::vector<core::RoomId> rooms,
        rt::StoreCallback<std::vector<std::pair<core::RoomId, rt::Ownership>>> done) override;
    void find_address(const core::NodeId& node,
                      rt::StoreCallback<std::optional<std::string>> done) override;

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace infra::postgres
