#include "core/ports/message_store.hpp"
#include "infra/messages/memory_message_store.hpp"
#include "net/reactor_factory.hpp"
#include "os/system_clock.hpp"
#include "os/system_random.hpp"

#include "conformance/message_store_harness.hpp"

#include <gtest/gtest.h>
#include <memory>
#include <utility>

namespace {

using core::ports::Admission;
using core::ports::RoomAccess;
using core::ports::RoomKind;

// What a call's handler reads of a room on the store a development chat_server keeps
// (ADR-0050): its recorded kind and its list, read only. The same laws on Postgres are in
// integration/message_store_test.cpp.
class MemoryStoreAccess : public ::testing::Test {
protected:
    void SetUp() override {
        auto reactor = net::make_reactor(net::ReactorKind::Epoll, clock_, 64);
        ASSERT_TRUE(reactor);
        reactor_ = std::move(*reactor);
        store_ = std::make_unique<infra::messages::MemoryMessageStore>(*reactor_);
    }

    core::ports::MessageResult<RoomAccess> access(const core::RoomId& room,
                                                  const core::UserId& user) {
        return ulw::test::ask<RoomAccess>(
            *reactor_, [&](auto done) { store_->access(room, user, std::move(done)); });
    }

    core::RoomId new_room() { return core::RoomId::generate(clock_, random_); }

    os::SystemClock clock_;
    os::SystemRandom random_;
    std::unique_ptr<net::IReactor> reactor_;
    std::unique_ptr<infra::messages::MemoryMessageStore> store_;
    const core::UserId alice_ = *core::UserId::parse("auth0|alice");
    const core::UserId bob_ = *core::UserId::parse("auth0|bob");
};

TEST_F(MemoryStoreAccess, ARoomNobodyRecordedHasNoKindAndNoMembersAndStaysSo) {
    const core::RoomId room = new_room();
    EXPECT_EQ(access(room, alice_), (RoomAccess{.kind = std::nullopt, .member = false}));
    // Looking recorded nothing: a direct chat's first join still records it as one.
    EXPECT_EQ(ulw::test::ask<Admission>(*reactor_,
                                        [&](auto done) {
                                            store_->admits(room, alice_, RoomKind::DirectChat,
                                                           std::move(done));
                                        }),
              Admission::NotMember);
    EXPECT_EQ(access(room, alice_), (RoomAccess{.kind = RoomKind::DirectChat, .member = false}));
}

TEST_F(MemoryStoreAccess, OnlyAListedUserIsAMember) {
    const core::RoomId room = new_room();
    ASSERT_TRUE(ulw::test::ask<void>(
        *reactor_, [&](auto done) { store_->add_member(room, alice_, std::move(done)); }));
    EXPECT_EQ(access(room, alice_), (RoomAccess{.kind = RoomKind::GroupChat, .member = true}));
    EXPECT_EQ(access(room, bob_), (RoomAccess{.kind = RoomKind::GroupChat, .member = false}));
    // Taken off the list, alice is no longer one; the kind stays recorded.
    ASSERT_TRUE(ulw::test::ask<void>(
        *reactor_, [&](auto done) { store_->remove_member(room, alice_, std::move(done)); }));
    EXPECT_EQ(access(room, alice_), (RoomAccess{.kind = RoomKind::GroupChat, .member = false}));
}

} // namespace
