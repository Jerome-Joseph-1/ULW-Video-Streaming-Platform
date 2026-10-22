#pragma once

#include "core/ports/message_store.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace ulw::test {

// A base for fakes of the message store whose tests are about something other than member lists
// changed by their users (ADR-0096): every such change and listing, a redaction (ADR-0135) and a
// relation's reads (ADR-0136) are answered Unavailable, at once.
class NoMembershipStore : public core::ports::IMessageStore {
public:
    void open_direct(const core::RoomId& /*room*/, const core::UserId& /*user*/,
                     const core::UserId& /*peer*/,
                     core::ports::MessageCallback<core::ports::MembershipChange> done) override {
        done(std::unexpected(core::ports::MessageStoreError::Unavailable));
    }
    void create_group(const core::RoomId& /*room*/, const core::UserId& /*creator*/,
                      std::vector<core::UserId> /*members*/,
                      core::ports::MessageCallback<core::ports::MembershipChange> done) override {
        done(std::unexpected(core::ports::MessageStoreError::Unavailable));
    }
    void add_members(const core::RoomId& /*room*/, const core::ports::Actor& /*actor*/,
                     std::vector<core::UserId> /*users*/,
                     core::ports::MessageCallback<core::ports::MembershipChange> done) override {
        done(std::unexpected(core::ports::MessageStoreError::Unavailable));
    }
    void expel(const core::RoomId& /*room*/, const core::UserId& /*actor*/,
               const core::UserId& /*user*/,
               core::ports::MessageCallback<core::ports::MembershipChange> done) override {
        done(std::unexpected(core::ports::MessageStoreError::Unavailable));
    }
    void close_direct(const core::RoomId& /*room*/,
                      core::ports::MessageCallback<core::ports::MembershipChange> done) override {
        done(std::unexpected(core::ports::MessageStoreError::Unavailable));
    }
    void leave_room(const core::RoomId& /*room*/, const core::UserId& /*user*/,
                    core::ports::MessageCallback<core::ports::MembershipChange> done) override {
        done(std::unexpected(core::ports::MessageStoreError::Unavailable));
    }
    void set_role(const core::RoomId& /*room*/, const core::UserId& /*user*/,
                  core::ports::MemberRole /*role*/,
                  core::ports::MessageCallback<core::ports::MembershipChange> done) override {
        done(std::unexpected(core::ports::MessageStoreError::Unavailable));
    }
    void rooms_of(const core::UserId& /*user*/, std::optional<core::RoomId> /*after*/,
                  std::size_t /*limit*/,
                  core::ports::MessageCallback<std::vector<core::ports::RoomEntry>> done) override {
        done(std::unexpected(core::ports::MessageStoreError::Unavailable));
    }
    void roster(const core::RoomId& /*room*/, const core::ports::Actor& /*asker*/,
                std::optional<core::UserId> /*after*/, std::size_t /*limit*/,
                core::ports::MessageCallback<core::ports::Roster> done) override {
        done(std::unexpected(core::ports::MessageStoreError::Unavailable));
    }
    void shared_with(const core::UserId& /*user*/, std::vector<core::UserId> /*others*/,
                     core::ports::MessageCallback<std::vector<core::UserId>> done) override {
        done(std::unexpected(core::ports::MessageStoreError::Unavailable));
    }
    void redact(const core::RoomId& /*room*/, std::uint64_t /*seq*/,
                const core::ports::Actor& /*actor*/,
                core::ports::MessageCallback<core::ports::RedactOutcome> done) override {
        done(std::unexpected(core::ports::MessageStoreError::Unavailable));
    }
    void
    relating(const core::RoomId& /*room*/, const core::ports::RelatingPage& /*page*/,
             std::size_t /*limit*/,
             core::ports::MessageCallback<std::vector<core::ports::StoredMessage>> done) override {
        done(std::unexpected(core::ports::MessageStoreError::Unavailable));
    }
    void
    rel_counts(const core::RoomId& /*room*/, std::vector<std::uint64_t> /*seqs*/,
               core::ports::RelType /*type*/,
               core::ports::MessageCallback<std::vector<core::ports::RelCount>> done) override {
        done(std::unexpected(core::ports::MessageStoreError::Unavailable));
    }
    void disconnect_user(const core::UserId& /*user*/,
                         core::ports::MessageCallback<void> done) override {
        done(std::unexpected(core::ports::MessageStoreError::Unavailable));
    }
    void mark_read(const core::RoomId& /*room*/, const core::UserId& /*user*/,
                   std::uint64_t /*seq*/,
                   core::ports::MessageCallback<std::optional<std::uint64_t>> done) override {
        done(std::unexpected(core::ports::MessageStoreError::Unavailable));
    }
    void
    read_marks(const core::RoomId& /*room*/,
               core::ports::MessageCallback<std::vector<core::ports::ReadMark>> done) override {
        done(std::unexpected(core::ports::MessageStoreError::Unavailable));
    }
};

} // namespace ulw::test
