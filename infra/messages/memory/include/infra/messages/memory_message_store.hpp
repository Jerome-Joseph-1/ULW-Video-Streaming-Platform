#pragma once

#include "core/ports/message_store.hpp"
#include "net/reactor.hpp"

#include <functional>
#include <map>
#include <set>
#include <unordered_map>
#include <vector>

namespace infra::messages {

// The message store without a database, for tests and single-process development. Same
// contract as the durable one: answers arrive on a later loop iteration, and sent_at keeps
// microseconds, as a timestamptz does. It keeps no sequence counter, so it has a writer of its
// own: whatever takes the seq appends the message here, and last_seq is the highest stored.
class MemoryMessageStore final : public core::ports::IMessageStore, public net::ITimerHandler {
public:
    explicit MemoryMessageStore(net::IReactor& reactor);
    ~MemoryMessageStore() override;
    MemoryMessageStore(const MemoryMessageStore&) = delete;
    MemoryMessageStore& operator=(const MemoryMessageStore&) = delete;
    MemoryMessageStore(MemoryMessageStore&&) = delete;
    MemoryMessageStore& operator=(MemoryMessageStore&&) = delete;

    // Stores a message under `seq` (from 1). Idempotent: the same seq with the same sender and
    // body succeeds and keeps the first sent_at; any other is Conflict. Over kMaxMessageBody is
    // TooLarge.
    void append(const core::RoomId& room, std::uint64_t seq, const core::UserId& sender,
                std::vector<std::byte> body, core::WallTime sent_at,
                core::ports::MessageCallback<void> done);
    void history_before(
        const core::RoomId& room, std::optional<std::uint64_t> before, std::size_t limit,
        core::ports::MessageCallback<std::vector<core::ports::StoredMessage>> done) override;
    void history_after(
        const core::RoomId& room, std::uint64_t after, std::size_t limit,
        core::ports::MessageCallback<std::vector<core::ports::StoredMessage>> done) override;
    void last_seq(const core::RoomId& room,
                  core::ports::MessageCallback<std::uint64_t> done) override;
    void add_member(const core::RoomId& room, const core::UserId& user,
                    core::ports::MessageCallback<void> done) override;
    void remove_member(const core::RoomId& room, const core::UserId& user,
                       core::ports::MessageCallback<void> done) override;
    void members(const core::RoomId& room, std::optional<core::UserId> after, std::size_t limit,
                 core::ports::MessageCallback<std::vector<core::UserId>> done) override;

    void on_timeout() noexcept override;

private:
    struct ByteOrder {
        bool operator()(const core::UserId& a, const core::UserId& b) const noexcept {
            return a.view() < b.view();
        }
    };
    using Room = std::map<std::uint64_t, core::ports::StoredMessage>;

    void defer(std::move_only_function<void() noexcept> fn);

    net::IReactor& reactor_;
    net::TimerId timer_;
    std::vector<std::move_only_function<void() noexcept>> pending_;
    std::unordered_map<core::RoomId, Room> rooms_;
    // Ordered bytewise, as the durable store lists them.
    std::unordered_map<core::RoomId, std::set<core::UserId, ByteOrder>> members_;
};

} // namespace infra::messages
