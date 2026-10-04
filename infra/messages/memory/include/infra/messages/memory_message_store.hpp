#pragma once

#include "core/ports/message_store.hpp"
#include "net/reactor.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
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

    // Stores a message under `seq` (from 1) and answers the seq it is stored under. As the
    // durable store does, a key the sender already used in the room is answered with that
    // message's seq when the body is the same, and is Conflict when it is not; nothing is
    // written either way. A seq already taken by another message is Conflict. Over
    // kMaxMessageBody is TooLarge. An ephemeral room's
    // (rt::is_ephemeral_room) message is not kept: only its seq counts, for last_seq.
    void append(const core::RoomId& room, std::uint64_t seq, const core::UserId& sender,
                std::string key, std::vector<std::byte> body, core::WallTime sent_at,
                core::ports::MessageCallback<std::uint64_t> done);
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
    using core::ports::IMessageStore::admits;
    void admits(const core::RoomId& room, const core::UserId& user, core::ports::RoomKind asked,
                core::ports::Recording recording,
                core::ports::MessageCallback<core::ports::Admission> done) override;
    void access(const core::RoomId& room, const core::UserId& user,
                core::ports::MessageCallback<core::ports::RoomAccess> done) override;
    void record_live(const core::RoomId& room, core::ports::MessageCallback<void> done) override;
    // Told of every member this store lists or takes off, after the answer of the call that did
    // it: nothing else changes this store's lists.
    void watch_members(core::ports::IMemberListener* listener) noexcept override {
        listener_ = listener;
    }
    void open_direct(const core::RoomId& room, const core::UserId& user, const core::UserId& peer,
                     core::ports::MessageCallback<core::ports::MembershipChange> done) override;
    void create_group(const core::RoomId& room, const core::UserId& creator,
                      std::vector<core::UserId> members,
                      core::ports::MessageCallback<core::ports::MembershipChange> done) override;
    void add_members(const core::RoomId& room, const core::ports::Actor& actor,
                     std::vector<core::UserId> users,
                     core::ports::MessageCallback<core::ports::MembershipChange> done) override;
    void expel(const core::RoomId& room, const core::UserId& actor, const core::UserId& user,
               core::ports::MessageCallback<core::ports::MembershipChange> done) override;
    void leave_room(const core::RoomId& room, const core::UserId& user,
                    core::ports::MessageCallback<core::ports::MembershipChange> done) override;
    void rooms_of(const core::UserId& user, std::optional<core::RoomId> after, std::size_t limit,
                  core::ports::MessageCallback<std::vector<core::ports::RoomEntry>> done) override;
    void roster(const core::RoomId& room, const core::ports::Actor& asker,
                std::optional<core::UserId> after, std::size_t limit,
                core::ports::MessageCallback<core::ports::Roster> done) override;
    void shared_with(const core::UserId& user, std::vector<core::UserId> others,
                     core::ports::MessageCallback<std::vector<core::UserId>> done) override;

    void on_timeout() noexcept override;

private:
    core::ports::IMemberListener* listener_ = nullptr;
    struct ByteOrder {
        bool operator()(const core::UserId& a, const core::UserId& b) const noexcept {
            return a.view() < b.view();
        }
    };
    struct Room {
        std::map<std::uint64_t, core::ports::StoredMessage> messages;
        // (sender, key) of every stored message, and its seq.
        std::map<std::pair<std::string, std::string>, std::uint64_t> keys;
        // The highest seq appended to an ephemeral room, which keeps no messages.
        std::uint64_t last = 0;
    };

    using Members = std::map<core::UserId, core::ports::MemberRole, ByteOrder>;

    void defer(std::move_only_function<void() noexcept> fn);
    // Answers `done` with `change`, then tells the listener of each id in it: added or removed.
    void answer_change(core::ports::MembershipChange change, bool added, const core::RoomId& room,
                       core::ports::MessageCallback<core::ports::MembershipChange> done);
    // Lists `users` (the first with `first_role`) in a room that lists nobody yet: what
    // open_direct and create_group do, once the room is recorded as `kind`.
    void create_listed(const core::RoomId& room, core::ports::RoomKind kind,
                       const core::UserId& asker, std::vector<core::UserId> users,
                       core::ports::MemberRole first_role,
                       core::ports::MessageCallback<core::ports::MembershipChange> done);

    net::IReactor& reactor_;
    net::TimerId timer_;
    std::vector<std::move_only_function<void() noexcept>> pending_;
    std::unordered_map<core::RoomId, Room> rooms_;
    // Ordered bytewise, as the durable store lists them, with each member's role.
    std::unordered_map<core::RoomId, Members> members_;
    // As recorded by each room's first join or member, or by record_live.
    std::unordered_map<core::RoomId, core::ports::RoomKind> kinds_;
};

} // namespace infra::messages
