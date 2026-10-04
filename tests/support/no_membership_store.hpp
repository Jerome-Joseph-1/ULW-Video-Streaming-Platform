#pragma once

#include "core/ports/message_store.hpp"

#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

namespace ulw::test {

// A base for fakes of the message store whose tests are about something other than member lists
// changed by their users (ADR-0096): every such change and listing is answered Unavailable, at
// once.
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
    void leave_room(const core::RoomId& /*room*/, const core::UserId& /*user*/,
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
};

} // namespace ulw::test
