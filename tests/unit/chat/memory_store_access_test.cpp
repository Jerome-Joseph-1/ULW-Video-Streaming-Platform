#include "core/ports/message_store.hpp"
#include "infra/messages/memory_message_store.hpp"
#include "net/reactor_factory.hpp"
#include "os/system_clock.hpp"
#include "os/system_random.hpp"

#include "conformance/message_store_harness.hpp"

#include <chrono>
#include <cstdint>
#include <gtest/gtest.h>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

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

// ADR-0135 on the store a development chat_server keeps: its sender, an admin of the room or the
// service blanks a message, and nobody else; the same laws on Postgres are in
// integration/message_store_test.cpp.
TEST_F(MemoryStoreAccess, AMessageIsRedactedByItsSenderAnAdminOrTheServiceAndStaysSo) {
    using core::ports::RedactOutcome;
    const core::RoomId room = new_room();
    const core::UserId carol = *core::UserId::parse("auth0|carol");
    for (const core::UserId& user : {alice_, bob_, carol}) {
        ASSERT_TRUE(ulw::test::ask<void>(
            *reactor_, [&](auto done) { store_->add_member(room, user, std::move(done)); }));
    }
    ASSERT_TRUE(ulw::test::ask<core::ports::MembershipChange>(*reactor_, [&](auto done) {
        store_->set_role(room, carol, core::ports::MemberRole::Admin, std::move(done));
    }));
    const auto append = [&](std::uint64_t seq, const core::UserId& sender, std::string key,
                            std::vector<std::byte> body) {
        return ulw::test::ask<std::uint64_t>(*reactor_, [&](auto done) {
            store_->append(room, seq, sender, std::move(key), std::move(body),
                           std::chrono::system_clock::now(), std::move(done));
        });
    };
    for (std::uint64_t seq = 1; seq <= 3; ++seq) {
        ASSERT_TRUE(append(seq, alice_, "k" + std::to_string(seq), {std::byte{7}}));
    }
    const auto redact = [&](std::uint64_t seq, const core::ports::Actor& actor) {
        return ulw::test::ask<RedactOutcome>(
            *reactor_, [&](auto done) { store_->redact(room, seq, actor, std::move(done)); });
    };
    EXPECT_EQ(redact(1, bob_), RedactOutcome::NotAllowed);
    EXPECT_EQ(redact(1, alice_), RedactOutcome::Done);
    EXPECT_EQ(redact(1, alice_), RedactOutcome::Done);
    EXPECT_EQ(redact(2, carol), RedactOutcome::Done);
    EXPECT_EQ(redact(3, core::ports::Actor::service()), RedactOutcome::Done);
    EXPECT_EQ(redact(4, alice_), RedactOutcome::NotFound);
    EXPECT_EQ(
        ulw::test::ask<RedactOutcome>(
            *reactor_, [&](auto done) { store_->redact(new_room(), 1, alice_, std::move(done)); }),
        RedactOutcome::NotFound);
    const auto page = ulw::test::ask<std::vector<core::ports::StoredMessage>>(
        *reactor_, [&](auto done) { store_->history_after(room, 0, 10, std::move(done)); });
    ASSERT_TRUE(page);
    ASSERT_EQ(page->size(), 3U);
    for (const core::ports::StoredMessage& m : *page) {
        EXPECT_TRUE(m.redacted);
        EXPECT_TRUE(m.body.empty());
        EXPECT_EQ(m.sender, alice_);
    }
    // A repeat of the send is answered with its seq, whatever its body, and restores nothing.
    EXPECT_EQ(append(9, alice_, "k1", {std::byte{8}}), 1U);
}

// ADR-0136 on the store a development chat_server keeps: a message's relation is kept with it,
// listed by the message it relates to and counted, and goes when it is redacted; a relation to a
// later message is not kept. The same laws on Postgres are in integration/message_store_test.cpp.
TEST_F(MemoryStoreAccess, ARelationIsKeptListedAndCountedAndGoesWithARedaction) {
    using core::ports::Relation;
    using core::ports::RelType;
    using core::ports::StoredMessage;
    const core::RoomId room = new_room();
    const auto append = [&](std::uint64_t seq, std::optional<Relation> rel) {
        return ulw::test::ask<std::uint64_t>(*reactor_, [&](auto done) {
            store_->append(room, seq, alice_, "k" + std::to_string(seq), {std::byte{1}},
                           std::chrono::system_clock::now(), std::move(done), false, rel);
        });
    };
    ASSERT_TRUE(append(1, std::nullopt));
    ASSERT_TRUE(append(2, Relation{.type = RelType::Reply, .seq = 1}));
    ASSERT_TRUE(append(3, Relation{.type = RelType::React, .seq = 1}));
    ASSERT_TRUE(append(4, Relation{.type = RelType::Reply, .seq = 1}));
    // A relation to a message after it is not kept.
    ASSERT_TRUE(append(5, Relation{.type = RelType::Reply, .seq = 9}));
    const auto relating = [&](core::ports::RelatingPage page, std::size_t limit = 10) {
        const auto got = ulw::test::ask<std::vector<StoredMessage>>(
            *reactor_, [&](auto done) { store_->relating(room, page, limit, std::move(done)); });
        std::vector<std::uint64_t> seqs;
        for (const StoredMessage& m : got.value_or(std::vector<StoredMessage>{})) {
            seqs.push_back(m.seq);
        }
        return seqs;
    };
    EXPECT_EQ(
        relating({.to = 1, .type = std::nullopt, .before = std::nullopt, .after = std::nullopt}),
        (std::vector<std::uint64_t>{4, 3, 2}));
    EXPECT_EQ(relating({.to = 1, .type = RelType::Reply, .before = 4, .after = std::nullopt}),
              (std::vector<std::uint64_t>{2}));
    EXPECT_EQ(relating({.to = 1, .type = RelType::Reply, .before = std::nullopt, .after = 0}, 1),
              (std::vector<std::uint64_t>{2}));
    EXPECT_TRUE(
        relating({.to = 9, .type = std::nullopt, .before = std::nullopt, .after = std::nullopt})
            .empty());
    const auto counts = [&](std::vector<std::uint64_t> seqs, RelType type) {
        return ulw::test::ask<std::vector<core::ports::RelCount>>(*reactor_, [&](auto done) {
            store_->rel_counts(room, std::move(seqs), type, std::move(done));
        });
    };
    EXPECT_EQ(counts({9, 1, 1}, RelType::Reply),
              (std::vector<core::ports::RelCount>{{.seq = 1, .count = 2, .latest = 4},
                                                  {.seq = 9, .count = 0, .latest = 0}}));
    ASSERT_EQ(ulw::test::ask<core::ports::RedactOutcome>(
                  *reactor_, [&](auto done) { store_->redact(room, 4, alice_, std::move(done)); }),
              core::ports::RedactOutcome::Done);
    EXPECT_EQ(counts({1}, RelType::Reply),
              (std::vector<core::ports::RelCount>{{.seq = 1, .count = 1, .latest = 2}}));
    EXPECT_EQ(counts({1}, RelType::Reply), counts({1}, RelType::Reply));
    EXPECT_TRUE(ulw::test::ask<std::vector<core::ports::RelCount>>(
                    *reactor_,
                    [&](auto done) {
                        store_->rel_counts(new_room(), {1}, RelType::Reply, std::move(done));
                    })
                    ->front()
                    .count == 0);
}

} // namespace
