// Laws every IMessageStore must obey, run against the in-memory store and against Postgres
// (skipped when no Postgres is reachable, as the integration suites are). Messages reach each
// store the way they do in use: through the room store's fenced write on Postgres, and through
// the in-memory store's own append under a counter kept here.
#include "infra/messages/memory_message_store.hpp"
#include "infra/postgres/message_store.hpp"
#include "infra/postgres/room_store.hpp"
#include "net/offload_pool.hpp"
#include "net/reactor_factory.hpp"
#include "os/system_clock.hpp"
#include "os/system_random.hpp"
#include "rt/room_store.hpp"

#include "integration/postgres_harness.hpp"
#include "message_store_harness.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <format>
#include <functional>
#include <gtest/gtest.h>
#include <map>
#include <memory>
#include <optional>
#include <ostream>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

namespace {

using core::ports::Admission;
using core::ports::kMaxHistoryBytes;
using core::ports::kMaxHistoryRows;
using core::ports::kMaxMessageBody;
using core::ports::MessageCallback;
using core::ports::MessageResult;
using core::ports::MessageStoreError;
using core::ports::StoredMessage;
using Page = std::vector<StoredMessage>;

// A store and the loop that drives it.
class Backend {
public:
    Backend() = default;
    Backend(const Backend&) = delete;
    Backend& operator=(const Backend&) = delete;
    Backend(Backend&&) = delete;
    Backend& operator=(Backend&&) = delete;
    virtual ~Backend() = default;

    [[nodiscard]] net::IReactor& reactor() { return *reactor_; }
    [[nodiscard]] virtual core::ports::IMessageStore& store() = 0;
    // Makes `room` writable by this test, as its owner.
    virtual void open(const core::RoomId& room) = 0;
    // Returns once changes to member lists reach `listener`, which watches the store from here on:
    // at once in memory, once the listening session has said so (a resync) on Postgres.
    virtual void watch(core::ports::IMemberListener& listener, const int& resyncs) {
        store().watch_members(&listener);
        (void)resyncs;
    }
    // Stores a message under the room's next seq and answers that seq.
    virtual void write(const core::RoomId& room, const core::UserId& sender, std::string key,
                       std::vector<std::byte> body, MessageCallback<std::uint64_t> done) = 0;

protected:
    [[nodiscard]] bool make_reactor() {
        auto reactor = net::make_reactor(net::ReactorKind::Epoll, clock_, 1024);
        if (!reactor) {
            return false;
        }
        reactor_ = std::move(*reactor);
        return true;
    }

    os::SystemClock clock_;
    std::unique_ptr<net::IReactor> reactor_;
};

class MemoryBackend final : public Backend {
public:
    MemoryBackend() {
        if (make_reactor()) {
            store_.emplace(*reactor_);
        }
    }
    core::ports::IMessageStore& store() override { return *store_; }
    void open(const core::RoomId& /*room*/) override {}

    void write(const core::RoomId& room, const core::UserId& sender, std::string key,
               std::vector<std::byte> body, MessageCallback<std::uint64_t> done) override {
        const std::uint64_t seq = next_[room] + 1;
        store_->append(room, seq, sender, std::move(key), std::move(body),
                       std::chrono::system_clock::now(),
                       [this, room, seq,
                        done = std::move(done)](MessageResult<std::uint64_t> r) mutable noexcept {
                           // A repeat is answered with the seq it already has, and takes none.
                           if (r && *r == seq) {
                               next_[room] = seq;
                           }
                           done(r);
                       });
    }

private:
    std::optional<infra::messages::MemoryMessageStore> store_;
    // The counter a room store would keep.
    std::map<core::RoomId, std::uint64_t> next_;
};

class PostgresBackend final : public Backend {
public:
    // Null when the test was skipped or failed setting up.
    static std::unique_ptr<Backend> make() {
        auto backend = std::make_unique<PostgresBackend>();
        ulw::test::ScratchDatabase::open(backend->db_);
        if (!backend->db_ || !backend->make_reactor()) {
            return nullptr;
        }
        auto offload = net::OffloadPool::create(*backend->reactor_, 1);
        if (!offload) {
            return nullptr;
        }
        backend->offload_ = std::move(*offload);
        auto store = infra::postgres::PgMessageStore::create(
            *backend->reactor_, *backend->offload_, {.conninfo = backend->db_->conninfo()});
        if (!store) {
            ADD_FAILURE() << store.error();
            return nullptr;
        }
        backend->store_ = std::move(*store);
        auto rooms = infra::postgres::PgRoomStore::create(*backend->reactor_, *backend->offload_,
                                                          {.conninfo = backend->db_->conninfo()});
        if (!rooms) {
            ADD_FAILURE() << rooms.error();
            return nullptr;
        }
        backend->rooms_ = std::move(*rooms);
        return backend;
    }

    ~PostgresBackend() override {
        offload_.reset();
        rooms_.reset();
        store_.reset();
    }
    PostgresBackend() = default;
    PostgresBackend(const PostgresBackend&) = delete;
    PostgresBackend& operator=(const PostgresBackend&) = delete;
    PostgresBackend(PostgresBackend&&) = delete;
    PostgresBackend& operator=(PostgresBackend&&) = delete;

    core::ports::IMessageStore& store() override { return *store_; }

    void watch(core::ports::IMemberListener& listener, const int& resyncs) override {
        store().watch_members(&listener);
        ASSERT_TRUE(ulw::test::pump_until(*reactor_, [&] { return resyncs > 0; }))
            << "the store never listened";
    }

    void open(const core::RoomId& room) override {
        const auto owner = ulw::test::ask_store<rt::Ownership>(
            *reactor_, [&](auto done) { rooms_->resolve(room, kNode, std::move(done)); });
        ASSERT_TRUE(owner);
        generations_[room] = owner->generation;
    }

    void write(const core::RoomId& room, const core::UserId& sender, std::string key,
               std::vector<std::byte> body, MessageCallback<std::uint64_t> done) override {
        rooms_->append(room, generations_.at(room), ulw::test::outgoing(sender, key, body),
                       [done = std::move(done)](
                           rt::StoreResult<std::optional<std::uint64_t>> r) mutable noexcept {
                           if (!r) {
                               done(std::unexpected(r.error() == rt::StoreError::Conflict
                                                        ? MessageStoreError::Conflict
                                                        : MessageStoreError::Unavailable));
                           } else if (!*r) {
                               // This test is the room's only owner; fenced would be a broken
                               // store.
                               done(std::unexpected(MessageStoreError::Corrupt));
                           } else {
                               done(**r);
                           }
                       });
    }

private:
    static inline const core::NodeId kNode = *core::NodeId::parse("chat-a");

    std::unique_ptr<ulw::test::ScratchDatabase> db_;
    std::unique_ptr<net::OffloadPool> offload_;
    std::unique_ptr<infra::postgres::PgMessageStore> store_;
    std::unique_ptr<infra::postgres::PgRoomStore> rooms_;
    std::map<core::RoomId, std::uint64_t> generations_;
};

struct BackendFactory {
    std::string name;
    std::function<std::unique_ptr<Backend>()> make;
    friend void PrintTo(const BackendFactory& f, std::ostream* os) { *os << f.name; }
};

core::UserId user(std::string_view id) {
    return *core::UserId::parse(id);
}

std::vector<std::byte> bytes(std::string_view text) {
    std::vector<std::byte> out(text.size());
    std::ranges::transform(text, out.begin(), [](char c) { return static_cast<std::byte>(c); });
    return out;
}

// Whole seconds, which every store keeps exactly.
core::WallTime at(std::int64_t second) {
    return core::WallTime{std::chrono::seconds{1'790'000'000 + second}};
}

class MessageStoreConformance : public ::testing::TestWithParam<BackendFactory> {
protected:
    void SetUp() override {
        backend_ = GetParam().make();
        if (IsSkipped() || HasFailure()) {
            return;
        }
        ASSERT_NE(backend_, nullptr) << "backend unavailable";
    }

    core::ports::IMessageStore& store() { return backend_->store(); }

    template <class T, class Call> MessageResult<T> ask(Call call) {
        return ulw::test::ask<T>(backend_->reactor(), std::move(call));
    }

    // Each call a message of its own, under a key never used before.
    MessageResult<std::uint64_t> write(const core::RoomId& room, const core::UserId& sender,
                                       std::vector<std::byte> body) {
        return ask<std::uint64_t>([&](auto done) {
            backend_->write(room, sender, std::format("m{}", ++keys_), std::move(body),
                            std::move(done));
        });
    }

    // Messages 1..count, one at a time as an owner writes a room's
    // messages, each carrying body_of(its seq).
    void write_many(const core::RoomId& room, std::uint64_t count) {
        for (std::uint64_t seq = 1; seq <= count; ++seq) {
            ASSERT_EQ(write(room, alice_, body_of(seq)), seq);
        }
    }

    static std::vector<std::byte> body_of(std::uint64_t seq) {
        return bytes("message " + std::to_string(seq));
    }

    MessageResult<Page> before(const core::RoomId& room, std::optional<std::uint64_t> cursor,
                               std::size_t limit) {
        return ask<Page>(
            [&](auto done) { store().history_before(room, cursor, limit, std::move(done)); });
    }

    MessageResult<Page> after(const core::RoomId& room, std::uint64_t cursor, std::size_t limit) {
        return ask<Page>(
            [&](auto done) { store().history_after(room, cursor, limit, std::move(done)); });
    }

    MessageResult<std::uint64_t> last_seq(const core::RoomId& room) {
        return ask<std::uint64_t>([&](auto done) { store().last_seq(room, std::move(done)); });
    }

    MessageResult<std::vector<core::UserId>>
    members(const core::RoomId& room, std::optional<core::UserId> from, std::size_t limit) {
        return ask<std::vector<core::UserId>>(
            [&](auto done) { store().members(room, from, limit, std::move(done)); });
    }

    static std::vector<std::uint64_t> seqs(const Page& page) {
        std::vector<std::uint64_t> out;
        for (const StoredMessage& m : page) {
            out.push_back(m.seq);
        }
        return out;
    }

    // A room this test owns.
    core::RoomId new_room() {
        const core::RoomId room = core::RoomId::generate(clock_, random_);
        backend_->open(room);
        return room;
    }

    // A stream's room nothing has created yet: on Postgres it has no room_state row, as a stream's
    // room has before the server opens its chat. new_room() creates one, as a group chat, and a
    // room created closed cannot be opened afterwards. Its id is version 8 and tagged 01, as only a
    // stream's chat has (core::ports::is_stream_chat); the rest of it is random, like a digest's.
    core::RoomId unopened_room() {
        std::string text = core::RoomId::generate(clock_, random_).to_string();
        text[14] = '8';
        text.replace(0, 2, "01");
        return *core::RoomId::parse(text);
    }

    os::SystemClock clock_;
    os::SystemRandom random_;
    std::unique_ptr<Backend> backend_;
    std::uint64_t keys_ = 0;
    const core::UserId alice_ = user("auth0|alice");
    const core::UserId bob_ = user("auth0|bob");
};

TEST_P(MessageStoreConformance, AMessageComesBackWithEveryByteItWasStoredWith) {
    const core::RoomId room = new_room();
    std::vector<std::byte> every(256);
    for (std::size_t i = 0; i < every.size(); ++i) {
        every[i] = static_cast<std::byte>(i);
    }
    ASSERT_EQ(ask<std::uint64_t>([&](auto done) {
                  backend_->write(room, alice_, "01J9Z-key_0", every, std::move(done));
              }),
              1U);
    ASSERT_EQ(write(room, bob_, {}), 2U);

    const auto page = before(room, std::nullopt, 10);
    ASSERT_TRUE(page);
    ASSERT_EQ(page->size(), 2U);
    EXPECT_EQ((*page)[0].seq, 2U);
    EXPECT_EQ((*page)[0].sender, bob_);
    EXPECT_TRUE((*page)[0].body.empty());
    EXPECT_EQ((*page)[1].seq, 1U);
    EXPECT_EQ((*page)[1].sender, alice_);
    EXPECT_EQ((*page)[1].key, "01J9Z-key_0");
    EXPECT_EQ((*page)[1].body, every);
}

TEST_P(MessageStoreConformance, SentAtIsWhenTheMessageWasStoredToTheMicrosecond) {
    const core::RoomId room = new_room();
    ASSERT_TRUE(write(room, alice_, bytes("first")));
    ASSERT_TRUE(write(room, alice_, bytes("second")));
    const auto page = after(room, 0, 10);
    ASSERT_TRUE(page);
    ASSERT_EQ(page->size(), 2U);
    for (const StoredMessage& m : *page) {
        EXPECT_EQ(m.sent_at, std::chrono::floor<std::chrono::microseconds>(m.sent_at));
        // Set by the store, and never left at the epoch.
        EXPECT_NE(m.sent_at, core::WallTime{});
    }
    EXPECT_LE((*page)[0].sent_at, (*page)[1].sent_at);
}

TEST_P(MessageStoreConformance, HistoryBeforeGoesBackFromTheCursorNewestFirst) {
    const core::RoomId room = new_room();
    ASSERT_NO_FATAL_FAILURE(write_many(room, 10));
    const auto newest = before(room, std::nullopt, 3);
    ASSERT_TRUE(newest);
    EXPECT_EQ(seqs(*newest), (std::vector<std::uint64_t>{10, 9, 8}));
    const auto older = before(room, 8, 3);
    ASSERT_TRUE(older);
    EXPECT_EQ(seqs(*older), (std::vector<std::uint64_t>{7, 6, 5}));
    EXPECT_EQ(older->front().body, body_of(7));
    const auto oldest = before(room, 2, 3);
    ASSERT_TRUE(oldest);
    EXPECT_EQ(seqs(*oldest), (std::vector<std::uint64_t>{1}));
}

TEST_P(MessageStoreConformance, HistoryAfterResumesFromTheCursorOldestFirst) {
    const core::RoomId room = new_room();
    ASSERT_NO_FATAL_FAILURE(write_many(room, 10));
    const auto first = after(room, 0, 3);
    ASSERT_TRUE(first);
    EXPECT_EQ(seqs(*first), (std::vector<std::uint64_t>{1, 2, 3}));
    const auto resumed = after(room, 8, 3);
    ASSERT_TRUE(resumed);
    EXPECT_EQ(seqs(*resumed), (std::vector<std::uint64_t>{9, 10}));
    EXPECT_EQ(resumed->back().body, body_of(10));
    const auto caught_up = after(room, 10, 3);
    ASSERT_TRUE(caught_up);
    EXPECT_TRUE(caught_up->empty());
}

TEST_P(MessageStoreConformance, AZeroLimitIsAnEmptyPage) {
    const core::RoomId room = new_room();
    ASSERT_TRUE(write(room, alice_, bytes("a")));
    EXPECT_EQ(before(room, std::nullopt, 0), Page{});
    EXPECT_EQ(after(room, 0, 0), Page{});
}

TEST_P(MessageStoreConformance, AskingForMoreThanTheRowCapGetsTheCap) {
    const core::RoomId room = new_room();
    ASSERT_NO_FATAL_FAILURE(write_many(room, kMaxHistoryRows + 10));
    const auto back = before(room, std::nullopt, 10'000);
    ASSERT_TRUE(back);
    EXPECT_EQ(back->size(), kMaxHistoryRows);
    EXPECT_EQ(back->front().seq, kMaxHistoryRows + 10);
    const auto forth = after(room, 0, 10'000);
    ASSERT_TRUE(forth);
    EXPECT_EQ(forth->size(), kMaxHistoryRows);
    EXPECT_EQ(forth->back().seq, kMaxHistoryRows);
}

TEST_P(MessageStoreConformance, APageEndsBeforeItsBodiesPassTheByteBound) {
    const core::RoomId room = new_room();
    // Four largest bodies fill the bound exactly; the fifth goes to
    // the next page.
    static_assert(4 * kMaxMessageBody == kMaxHistoryBytes);
    for (std::uint64_t seq = 1; seq <= 5; ++seq) {
        std::vector<std::byte> body(kMaxMessageBody, static_cast<std::byte>(seq));
        ASSERT_EQ(write(room, alice_, std::move(body)), seq);
    }
    const auto first = after(room, 0, 10);
    ASSERT_TRUE(first);
    EXPECT_EQ(seqs(*first), (std::vector<std::uint64_t>{1, 2, 3, 4}));
    const auto next = after(room, 4, 10);
    ASSERT_TRUE(next);
    EXPECT_EQ(seqs(*next), (std::vector<std::uint64_t>{5}));
    ASSERT_EQ(next->front().body.size(), kMaxMessageBody);
    EXPECT_EQ(next->front().body.back(), std::byte{5});
    const auto back = before(room, std::nullopt, 10);
    ASSERT_TRUE(back);
    EXPECT_EQ(seqs(*back), (std::vector<std::uint64_t>{5, 4, 3, 2}));
}

TEST_P(MessageStoreConformance, LastSeqIsZeroForAnEmptyRoomAndTheNewestOtherwise) {
    const core::RoomId room = new_room();
    EXPECT_EQ(last_seq(room), 0U);
    ASSERT_TRUE(write(room, alice_, bytes("a")));
    ASSERT_TRUE(write(room, alice_, bytes("b")));
    EXPECT_EQ(last_seq(room), 2U);
    EXPECT_EQ(last_seq(core::RoomId::generate(clock_, random_)), 0U);
}

TEST_P(MessageStoreConformance, AnEphemeralRoomCountsItsSeqsAndKeepsNoMessage) {
    std::string text = core::RoomId::generate(clock_, random_).to_string();
    text.replace(0, 2, "02");
    text[14] = '8';
    const core::RoomId room = *core::RoomId::parse(text);
    ASSERT_TRUE(rt::is_ephemeral_room(room));
    backend_->open(room);
    ASSERT_EQ(write(room, alice_, bytes("online")), 1U);
    ASSERT_EQ(write(room, bob_, bytes("online")), 2U);
    EXPECT_EQ(before(room, std::nullopt, 10), Page{});
    EXPECT_EQ(after(room, 0, 10), Page{});
    EXPECT_EQ(last_seq(room), 2U);
}

TEST_P(MessageStoreConformance, RoomsAreKeptApart) {
    const core::RoomId one = new_room();
    const core::RoomId two = new_room();
    ASSERT_EQ(write(one, alice_, bytes("in one")), 1U);
    ASSERT_EQ(write(two, bob_, bytes("in two")), 1U);
    ASSERT_EQ(write(two, bob_, bytes("in two again")), 2U);
    const auto page = before(one, std::nullopt, 10);
    ASSERT_TRUE(page);
    ASSERT_EQ(page->size(), 1U);
    EXPECT_EQ(page->front().body, bytes("in one"));
    EXPECT_EQ(last_seq(one), 1U);
    EXPECT_EQ(last_seq(two), 2U);
}

TEST_P(MessageStoreConformance, TenThousandMessagesPageBothWaysWithoutGapsOrDuplicates) {
    constexpr std::uint64_t kCount = 10'000;
    // Not a divisor of kCount, so the last page in each direction is
    // a short one.
    constexpr std::size_t kPage = 97;
    const core::RoomId room = new_room();
    ASSERT_NO_FATAL_FAILURE(write_many(room, kCount));

    std::vector<std::uint64_t> back;
    std::optional<std::uint64_t> cursor;
    for (;;) {
        const auto page = before(room, cursor, kPage);
        ASSERT_TRUE(page);
        if (page->empty()) {
            break;
        }
        for (const StoredMessage& m : *page) {
            ASSERT_EQ(m.body, body_of(m.seq));
            back.push_back(m.seq);
        }
        cursor = page->back().seq;
    }
    ASSERT_EQ(back.size(), kCount);
    for (std::uint64_t i = 0; i < kCount; ++i) {
        ASSERT_EQ(back[i], kCount - i);
    }

    std::vector<std::uint64_t> forth;
    std::uint64_t from = 0;
    for (;;) {
        const auto page = after(room, from, kPage);
        ASSERT_TRUE(page);
        if (page->empty()) {
            break;
        }
        for (const StoredMessage& m : *page) {
            forth.push_back(m.seq);
        }
        from = page->back().seq;
    }
    ASSERT_EQ(forth.size(), kCount);
    for (std::uint64_t i = 0; i < kCount; ++i) {
        ASSERT_EQ(forth[i], i + 1);
    }
}

TEST_P(MessageStoreConformance, AddingAMemberTwiceListsThemOnce) {
    const core::RoomId room = new_room();
    ASSERT_TRUE(ask<void>([&](auto done) { store().add_member(room, alice_, std::move(done)); }));
    ASSERT_TRUE(ask<void>([&](auto done) { store().add_member(room, alice_, std::move(done)); }));
    EXPECT_EQ(members(room, std::nullopt, 10), std::vector<core::UserId>{alice_});
    EXPECT_EQ(members(new_room(), std::nullopt, 10), std::vector<core::UserId>{});
}

TEST_P(MessageStoreConformance, RemovingAMemberIsIdempotent) {
    const core::RoomId room = new_room();
    ASSERT_TRUE(ask<void>([&](auto done) { store().add_member(room, alice_, std::move(done)); }));
    ASSERT_TRUE(ask<void>([&](auto done) { store().add_member(room, bob_, std::move(done)); }));
    for (int i = 0; i < 2; ++i) {
        ASSERT_TRUE(
            ask<void>([&](auto done) { store().remove_member(room, alice_, std::move(done)); }));
    }
    EXPECT_EQ(members(room, std::nullopt, 10), std::vector<core::UserId>{bob_});
}

TEST_P(MessageStoreConformance, MembersPageInTheByteOrderOfTheirIds) {
    const core::RoomId room = new_room();
    // A linguistic collation would put "a" before "B" and ignore the
    // punctuation.
    const std::vector<core::UserId> ordered{user("B"),   user("Z|1"),     user("a"),
                                            user("a.b"), user("auth0|x"), user("a|1")};
    for (const core::UserId& id : ordered | std::views::reverse) {
        ASSERT_TRUE(ask<void>([&](auto done) { store().add_member(room, id, std::move(done)); }));
    }
    std::vector<core::UserId> listed;
    std::optional<core::UserId> cursor;
    for (;;) {
        const auto page = members(room, cursor, 4);
        ASSERT_TRUE(page);
        if (page->empty()) {
            break;
        }
        listed.insert(listed.end(), page->begin(), page->end());
        cursor = page->back();
    }
    EXPECT_EQ(listed, ordered);
}

TEST_P(MessageStoreConformance, AKeyUsedAgainGetsItsSeqWithTheSameBodyAndIsAConflictWithAnother) {
    const core::RoomId room = new_room();
    const auto write_as = [&](const core::UserId& sender, std::string key, std::string_view body) {
        return ask<std::uint64_t>([&](auto done) {
            backend_->write(room, sender, std::move(key), bytes(body), std::move(done));
        });
    };
    ASSERT_EQ(write_as(alice_, "k1", "hello"), 1U);
    ASSERT_EQ(write_as(alice_, "k2", "later"), 2U);
    EXPECT_EQ(write_as(alice_, "k1", "hello"), 1U);
    EXPECT_EQ(write_as(alice_, "k1", "hello, edited"),
              MessageResult<std::uint64_t>{std::unexpected(MessageStoreError::Conflict)});
    EXPECT_EQ(write_as(bob_, "k1", "bob's own"), 3U);
    EXPECT_EQ(last_seq(room), 3U);
    const auto page = after(room, 0, 10);
    ASSERT_TRUE(page);
    ASSERT_EQ(page->size(), 3U);
    EXPECT_EQ(page->front().body, bytes("hello"));
}

TEST_P(MessageStoreConformance, AGroupRoomAdmitsOnlyItsMembersEvenWhileItHasNone) {
    const core::RoomId room = new_room();
    const auto admits = [&](const core::UserId& user) {
        return ask<Admission>([&](auto done) {
            store().admits(room, user, core::ports::RoomKind::GroupChat, std::move(done));
        });
    };
    EXPECT_EQ(admits(alice_), Admission::NotMember);
    EXPECT_EQ(admits(bob_), Admission::NotMember);
    ASSERT_TRUE(ask<void>([&](auto done) { store().add_member(room, alice_, std::move(done)); }));
    EXPECT_EQ(admits(alice_), Admission::Admitted);
    EXPECT_EQ(admits(bob_), Admission::NotMember);
    ASSERT_TRUE(
        ask<void>([&](auto done) { store().remove_member(room, alice_, std::move(done)); }));
    EXPECT_EQ(admits(alice_), Admission::NotMember);
}

TEST_P(MessageStoreConformance, AJoinThatAsksForLiveInARoomWithNoKindIsRefusedAndRecordsNothing) {
    const core::RoomId room = unopened_room();
    const auto admits = [&](const core::UserId& user, core::ports::RoomKind asked) {
        return ask<Admission>(
            [&](auto done) { store().admits(room, user, asked, std::move(done)); });
    };
    EXPECT_EQ(admits(alice_, core::ports::RoomKind::StreamLiveChat), Admission::NotLive);
    EXPECT_EQ(admits(bob_, core::ports::RoomKind::StreamLiveChat), Admission::NotLive);
    // Still unrecorded: the server can open it, which it could not had the joins closed it.
    ASSERT_TRUE(ask<void>([&](auto done) { store().record_live(room, std::move(done)); }));
    EXPECT_EQ(admits(alice_, core::ports::RoomKind::StreamLiveChat), Admission::Admitted);
}

TEST_P(MessageStoreConformance, ARoomRecordedLiveAdmitsAnyoneWhateverKindTheJoinNames) {
    const core::RoomId room = unopened_room();
    ASSERT_TRUE(ask<void>([&](auto done) { store().record_live(room, std::move(done)); }));
    // Recording it again changes nothing.
    ASSERT_TRUE(ask<void>([&](auto done) { store().record_live(room, std::move(done)); }));
    for (const auto asked : {core::ports::RoomKind::StreamLiveChat,
                             core::ports::RoomKind::GroupChat, core::ports::RoomKind::DirectChat}) {
        EXPECT_EQ(
            ask<Admission>([&](auto done) { store().admits(room, bob_, asked, std::move(done)); }),
            Admission::Admitted);
    }
}

TEST_P(MessageStoreConformance, AJoinCannotOpenARoomRecordedClosed) {
    const auto admits = [&](const core::RoomId& room, core::ports::RoomKind asked) {
        return ask<Admission>(
            [&](auto done) { store().admits(room, bob_, asked, std::move(done)); });
    };
    // Recorded by its first join.
    const core::RoomId group = new_room();
    EXPECT_EQ(admits(group, core::ports::RoomKind::DirectChat), Admission::NotMember);
    EXPECT_EQ(admits(group, core::ports::RoomKind::StreamLiveChat), Admission::NotLive);
    EXPECT_EQ(admits(group, core::ports::RoomKind::GroupChat), Admission::NotMember);
    // Recorded by its first member, before any join.
    const core::RoomId listed = new_room();
    ASSERT_TRUE(ask<void>([&](auto done) { store().add_member(listed, alice_, std::move(done)); }));
    EXPECT_EQ(admits(listed, core::ports::RoomKind::StreamLiveChat), Admission::NotLive);
    EXPECT_EQ(admits(listed, core::ports::RoomKind::GroupChat), Admission::NotMember);
}

// What a call's handler reads (ADR-0050): the recorded kind and the list, and nothing recorded.
TEST_P(MessageStoreConformance, AccessReadsTheRecordedKindAndTheListAndRecordsNothing) {
    using core::ports::RoomAccess;
    using core::ports::RoomKind;
    const auto access = [&](const core::RoomId& room, const core::UserId& user) {
        return ask<RoomAccess>([&](auto done) { store().access(room, user, std::move(done)); });
    };
    // Not new_room(): on Postgres the room plane's row records a group chat.
    const core::RoomId room = core::RoomId::generate(clock_, random_);
    EXPECT_EQ(access(room, alice_), (RoomAccess{.kind = std::nullopt, .member = false}));
    // Still unrecorded: a direct chat's first join records it as one.
    EXPECT_EQ(ask<Admission>([&](auto done) {
                  store().admits(room, alice_, RoomKind::DirectChat, std::move(done));
              }),
              Admission::NotMember);
    EXPECT_EQ(access(room, alice_), (RoomAccess{.kind = RoomKind::DirectChat, .member = false}));
    ASSERT_TRUE(ask<void>([&](auto done) { store().add_member(room, alice_, std::move(done)); }));
    EXPECT_EQ(access(room, alice_), (RoomAccess{.kind = RoomKind::DirectChat, .member = true}));
    EXPECT_EQ(access(room, bob_), (RoomAccess{.kind = RoomKind::DirectChat, .member = false}));
    // A room recorded by its first member is a group chat.
    const core::RoomId group = core::RoomId::generate(clock_, random_);
    ASSERT_TRUE(ask<void>([&](auto done) { store().add_member(group, bob_, std::move(done)); }));
    EXPECT_EQ(access(group, bob_), (RoomAccess{.kind = RoomKind::GroupChat, .member = true}));
}

// ADR-0070: the id says which rooms get a live chat's bounds, so only a stream's room is opened.
TEST_P(MessageStoreConformance, RecordLiveRefusesARoomThatIsNotAStreamsChat) {
    // A presence room is named too (version 8), under its own tag.
    std::string presence = unopened_room().to_string();
    presence.replace(0, 2, "02");
    const core::RoomId named = *core::RoomId::parse(presence);
    ASSERT_FALSE(core::ports::is_stream_chat(named));
    EXPECT_EQ(ask<void>([&](auto done) { store().record_live(named, std::move(done)); }),
              MessageResult<void>{std::unexpected(MessageStoreError::Conflict)});
    const core::RoomId room = core::RoomId::generate(clock_, random_);
    ASSERT_FALSE(core::ports::is_stream_chat(room));
    EXPECT_EQ(ask<void>([&](auto done) { store().record_live(room, std::move(done)); }),
              MessageResult<void>{std::unexpected(MessageStoreError::Conflict)});
    EXPECT_EQ(ask<Admission>([&](auto done) {
                  store().admits(room, bob_, core::ports::RoomKind::StreamLiveChat,
                                 std::move(done));
              }),
              Admission::NotLive);
    // Nothing was recorded: the room can still become a group chat.
    ASSERT_TRUE(ask<void>([&](auto done) { store().add_member(room, alice_, std::move(done)); }));
    EXPECT_EQ(ask<Admission>([&](auto done) {
                  store().admits(room, alice_, core::ports::RoomKind::GroupChat, std::move(done));
              }),
              Admission::Admitted);
}

TEST_P(MessageStoreConformance, RecordLiveRefusesARoomThatIsClosedOrListsMembers) {
    const auto record_live = [&](const core::RoomId& room) {
        return ask<void>([&](auto done) { store().record_live(room, std::move(done)); });
    };
    const MessageResult<void> conflict{std::unexpected(MessageStoreError::Conflict)};
    const core::RoomId joined = unopened_room();
    ASSERT_EQ(ask<Admission>([&](auto done) {
                  store().admits(joined, bob_, core::ports::RoomKind::GroupChat, std::move(done));
              }),
              Admission::NotMember);
    EXPECT_EQ(record_live(joined), conflict);
    const core::RoomId listed = unopened_room();
    ASSERT_TRUE(ask<void>([&](auto done) { store().add_member(listed, alice_, std::move(done)); }));
    EXPECT_EQ(record_live(listed), conflict);
    // A room whose members all left is still closed.
    ASSERT_TRUE(
        ask<void>([&](auto done) { store().remove_member(listed, alice_, std::move(done)); }));
    EXPECT_EQ(record_live(listed), conflict);
    EXPECT_EQ(ask<Admission>([&](auto done) {
                  store().admits(listed, bob_, core::ports::RoomKind::StreamLiveChat,
                                 std::move(done));
              }),
              Admission::NotLive);
}

// Member lists their users change (ADR-0096).

using core::ports::MemberEntry;
using core::ports::MemberRole;
using core::ports::MembershipChange;
using core::ports::MembershipOutcome;
using core::ports::RoomEntry;
using core::ports::RoomKind;
using core::ports::Roster;

// What a change told a listener, in the order told.
class Heard final : public core::ports::IMemberListener {
public:
    void on_member_removed(const core::RoomId& room, const core::UserId& user) noexcept override {
        changes.push_back(std::format("- {} {}", room.to_string(), user.view()));
    }
    void on_member_added(const core::RoomId& room, const core::UserId& user) noexcept override {
        changes.push_back(std::format("+ {} {}", room.to_string(), user.view()));
    }
    void on_member_role(const core::RoomId& room, const core::UserId& user,
                        core::ports::MemberRole role) noexcept override {
        changes.push_back(std::format("* {} {} {}", room.to_string(),
                                      role == MemberRole::Admin ? "admin" : "member", user.view()));
    }
    void on_members_resync() noexcept override { ++resyncs; }

    std::vector<std::string> changes;
    int resyncs = 0;
};

class MembershipConformance : public MessageStoreConformance {
protected:
    // A room named as the chat service names a direct chat (tag 03) or a group chat (tag 04),
    // the rest random like a digest's; nothing has recorded or created it.
    core::RoomId named_room(std::string_view tag) {
        std::string text = core::RoomId::generate(clock_, random_).to_string();
        text[14] = '8';
        text.replace(0, 2, tag);
        return *core::RoomId::parse(text);
    }

    MessageResult<MembershipChange> open_direct(const core::RoomId& room, const core::UserId& user,
                                                const core::UserId& peer) {
        return ask<MembershipChange>(
            [&](auto done) { store().open_direct(room, user, peer, std::move(done)); });
    }
    MessageResult<MembershipChange> create_group(const core::RoomId& room,
                                                 const core::UserId& creator,
                                                 std::vector<core::UserId> others) {
        return ask<MembershipChange>([&](auto done) {
            store().create_group(room, creator, std::move(others), std::move(done));
        });
    }
    MessageResult<MembershipChange> add(const core::RoomId& room, const core::ports::Actor& actor,
                                        std::vector<core::UserId> users) {
        return ask<MembershipChange>([&](auto done) {
            store().add_members(room, actor, std::move(users), std::move(done));
        });
    }
    MessageResult<MembershipChange> expel(const core::RoomId& room, const core::UserId& actor,
                                          const core::UserId& user) {
        return ask<MembershipChange>(
            [&](auto done) { store().expel(room, actor, user, std::move(done)); });
    }
    MessageResult<MembershipChange> leave(const core::RoomId& room, const core::UserId& user) {
        return ask<MembershipChange>(
            [&](auto done) { store().leave_room(room, user, std::move(done)); });
    }
    MessageResult<Roster> roster(const core::RoomId& room, const core::ports::Actor& asker,
                                 std::optional<core::UserId> after = std::nullopt,
                                 std::size_t limit = 100) {
        return ask<Roster>(
            [&](auto done) { store().roster(room, asker, after, limit, std::move(done)); });
    }
    MessageResult<std::vector<core::UserId>> shared(const core::UserId& user,
                                                    std::vector<core::UserId> others) {
        return ask<std::vector<core::UserId>>(
            [&](auto done) { store().shared_with(user, std::move(others), std::move(done)); });
    }
    MessageResult<std::vector<RoomEntry>>
    rooms_of(const core::UserId& user, std::optional<core::RoomId> after, std::size_t limit) {
        return ask<std::vector<RoomEntry>>(
            [&](auto done) { store().rooms_of(user, after, limit, std::move(done)); });
    }

    static MembershipChange done(std::vector<core::UserId> changed,
                                 std::optional<core::UserId> promoted = std::nullopt) {
        return {.outcome = MembershipOutcome::Done,
                .changed = std::move(changed),
                .promoted = promoted};
    }
    static MessageResult<MembershipChange> refused(MembershipOutcome outcome) {
        return MembershipChange{.outcome = outcome, .changed = {}, .promoted = std::nullopt};
    }

    // A group of alice (its admin), bob and carol.
    core::RoomId group() {
        const core::RoomId room = named_room("04");
        EXPECT_EQ(create_group(room, alice_, {carol_, bob_}), done({alice_, bob_, carol_}));
        return room;
    }

    const core::UserId carol_ = user("auth0|carol");
    const core::UserId dave_ = user("auth0|dave");
};

TEST_P(MembershipConformance, ADirectChatListsItsPairOnceWhicheverOfThemOpensIt) {
    const core::RoomId room = named_room("03");
    EXPECT_EQ(open_direct(room, alice_, bob_), done({alice_, bob_}));
    // The other side, and the same side again, find it as it is.
    EXPECT_EQ(open_direct(room, bob_, alice_), done({}));
    EXPECT_EQ(open_direct(room, alice_, bob_), done({}));
    EXPECT_EQ(roster(room, bob_),
              (Roster{.asker_listed = true,
                      .members = {{.user = alice_, .role = MemberRole::Member},
                                  {.user = bob_, .role = MemberRole::Member}}}));
    EXPECT_EQ(
        ask<core::ports::RoomAccess>([&](auto d) { store().access(room, alice_, std::move(d)); }),
        (core::ports::RoomAccess{.kind = RoomKind::DirectChat, .member = true}));
    // Nobody else is let in by it.
    EXPECT_EQ(open_direct(room, carol_, alice_), refused(MembershipOutcome::NotMember));
    EXPECT_EQ(members(room, std::nullopt, 10), (std::vector<core::UserId>{alice_, bob_}));
}

// A member an operator took off a direct chat is not put back by the other opening it again.
TEST_P(MembershipConformance, ADirectChatThatListsAnyoneIsLeftAsItIs) {
    const core::RoomId room = named_room("03");
    ASSERT_EQ(open_direct(room, alice_, bob_), done({alice_, bob_}));
    ASSERT_TRUE(ask<void>([&](auto d) { store().remove_member(room, bob_, std::move(d)); }));
    EXPECT_EQ(open_direct(room, alice_, bob_), done({}));
    EXPECT_EQ(open_direct(room, bob_, alice_), refused(MembershipOutcome::NotMember));
    EXPECT_EQ(members(room, std::nullopt, 10), std::vector<core::UserId>{alice_});
}

// A refused join records a room nobody opened yet (ADR-0054); opening it lists the pair.
TEST_P(MembershipConformance, ADirectChatAJoinRecordedFirstIsOpenedAllTheSame) {
    const core::RoomId room = named_room("03");
    ASSERT_EQ(ask<Admission>([&](auto d) {
                  store().admits(room, carol_, RoomKind::DirectChat, std::move(d));
              }),
              Admission::NotMember);
    EXPECT_EQ(open_direct(room, alice_, bob_), done({alice_, bob_}));
    EXPECT_EQ(ask<Admission>(
                  [&](auto d) { store().admits(room, bob_, RoomKind::DirectChat, std::move(d)); }),
              Admission::Admitted);
}

TEST_P(MembershipConformance, ARoomRecordedAsAnotherKindIsNeitherOpenedNorCreated) {
    const core::RoomId room = core::RoomId::generate(clock_, random_);
    ASSERT_TRUE(ask<void>([&](auto d) { store().add_member(room, alice_, std::move(d)); }));
    EXPECT_EQ(open_direct(room, alice_, bob_), refused(MembershipOutcome::WrongKind));
    const core::RoomId direct = core::RoomId::generate(clock_, random_);
    ASSERT_EQ(ask<Admission>([&](auto d) {
                  store().admits(direct, carol_, RoomKind::DirectChat, std::move(d));
              }),
              Admission::NotMember);
    EXPECT_EQ(create_group(direct, alice_, {bob_}), refused(MembershipOutcome::WrongKind));
    EXPECT_EQ(members(room, std::nullopt, 10), std::vector<core::UserId>{alice_});
    EXPECT_EQ(members(direct, std::nullopt, 10), std::vector<core::UserId>{});
}

TEST_P(MembershipConformance, AGroupsCreatorIsItsAdminAndACreateRepeatedChangesNothing) {
    const core::RoomId room = group();
    EXPECT_EQ(roster(room, carol_),
              (Roster{.asker_listed = true,
                      .members = {{.user = alice_, .role = MemberRole::Admin},
                                  {.user = bob_, .role = MemberRole::Member},
                                  {.user = carol_, .role = MemberRole::Member}}}));
    // A repeat, with whatever list, lists nobody more.
    EXPECT_EQ(create_group(room, alice_, {dave_}), done({}));
    EXPECT_EQ(create_group(room, dave_, {}), refused(MembershipOutcome::NotMember));
    EXPECT_EQ(members(room, std::nullopt, 10), (std::vector<core::UserId>{alice_, bob_, carol_}));
    // A group of one is a group.
    const core::RoomId alone = named_room("04");
    EXPECT_EQ(create_group(alone, dave_, {}), done({dave_}));
}

TEST_P(MembershipConformance, OnlyAGroupsAdminAddsAndThoseListedAlreadyStayAsTheyAre) {
    const core::RoomId room = group();
    EXPECT_EQ(add(room, bob_, {dave_}), refused(MembershipOutcome::NotAdmin));
    EXPECT_EQ(add(room, dave_, {dave_}), refused(MembershipOutcome::NotMember));
    EXPECT_EQ(add(named_room("04"), alice_, {dave_}), refused(MembershipOutcome::NotMember));
    EXPECT_EQ(add(room, alice_, {dave_, bob_}), done({dave_}));
    EXPECT_EQ(add(room, alice_, {dave_}), done({}));
    EXPECT_EQ(roster(room, alice_)->members.back(),
              (MemberEntry{.user = dave_, .role = MemberRole::Member}));
    // A direct chat's pair never changes.
    const core::RoomId direct = named_room("03");
    ASSERT_EQ(open_direct(direct, alice_, bob_), done({alice_, bob_}));
    EXPECT_EQ(add(direct, alice_, {carol_}), refused(MembershipOutcome::NotGroup));
    EXPECT_EQ(members(direct, std::nullopt, 10), (std::vector<core::UserId>{alice_, bob_}));
}

TEST_P(MembershipConformance, AGroupHoldsAtMostItsCapAndAnAddPastItAddsNobody) {
    const core::RoomId room = group();
    std::vector<core::UserId> batch;
    std::size_t next = 0;
    const auto fill = [&](std::size_t n) {
        batch.clear();
        for (std::size_t i = 0; i < n; ++i) {
            batch.push_back(user(std::format("u{:03}", next++)));
        }
        return batch;
    };
    ASSERT_EQ(add(room, alice_, fill(50))->changed.size(), 50U);
    ASSERT_EQ(add(room, alice_, fill(core::ports::kMaxGroupMembers - 53))->changed.size(),
              core::ports::kMaxGroupMembers - 53);
    EXPECT_EQ(members(room, std::nullopt, 1000)->size(), core::ports::kMaxGroupMembers);
    EXPECT_EQ(add(room, alice_, fill(2)), refused(MembershipOutcome::Full));
    EXPECT_EQ(members(room, std::nullopt, 1000)->size(), core::ports::kMaxGroupMembers);
    // Naming only those listed already fits.
    EXPECT_EQ(add(room, alice_, {bob_}), done({}));
}

TEST_P(MembershipConformance, OnlyAGroupsAdminRemovesOthers) {
    const core::RoomId room = group();
    EXPECT_EQ(expel(room, bob_, carol_), refused(MembershipOutcome::NotAdmin));
    EXPECT_EQ(expel(room, dave_, carol_), refused(MembershipOutcome::NotMember));
    EXPECT_EQ(expel(room, alice_, carol_), done({carol_}));
    EXPECT_EQ(expel(room, alice_, carol_), done({}));
    EXPECT_EQ(members(room, std::nullopt, 10), (std::vector<core::UserId>{alice_, bob_}));
    const core::RoomId direct = named_room("03");
    ASSERT_EQ(open_direct(direct, alice_, bob_), done({alice_, bob_}));
    EXPECT_EQ(expel(direct, alice_, bob_), refused(MembershipOutcome::NotGroup));
}

TEST_P(MembershipConformance, WhenTheLastAdminLeavesTheFirstMemberLeftBecomesOne) {
    const core::RoomId room = group();
    EXPECT_EQ(leave(room, carol_), done({carol_}));
    // Removing oneself is leaving.
    EXPECT_EQ(expel(room, alice_, alice_), done({alice_}, bob_));
    EXPECT_EQ(roster(room, bob_), (Roster{.asker_listed = true,
                                          .members = {{.user = bob_, .role = MemberRole::Admin}}}));
    EXPECT_EQ(leave(room, carol_), refused(MembershipOutcome::NotMember));
    // The last one out leaves a list of nobody, which admits nobody.
    EXPECT_EQ(leave(room, bob_), done({bob_}));
    EXPECT_EQ(ask<Admission>(
                  [&](auto d) { store().admits(room, bob_, RoomKind::GroupChat, std::move(d)); }),
              Admission::NotMember);
    const core::RoomId direct = named_room("03");
    ASSERT_EQ(open_direct(direct, alice_, bob_), done({alice_, bob_}));
    EXPECT_EQ(leave(direct, alice_), refused(MembershipOutcome::NotGroup));
    EXPECT_EQ(leave(named_room("04"), alice_), refused(MembershipOutcome::NotMember));
}

TEST_P(MembershipConformance, AMembersOwnRoomsPageInIdOrderWithTheirKindRoleAndPeer) {
    const core::RoomId direct = named_room("03");
    ASSERT_EQ(open_direct(direct, alice_, bob_), done({alice_, bob_}));
    const core::RoomId mine = group();
    const core::RoomId theirs = named_room("04");
    ASSERT_EQ(create_group(theirs, bob_, {alice_}), done({alice_, bob_}));
    // Listed by an operator, with no kind recorded by anything else: a group chat.
    const core::RoomId listed = core::RoomId::generate(clock_, random_);
    ASSERT_TRUE(ask<void>([&](auto d) { store().add_member(listed, alice_, std::move(d)); }));
    ASSERT_TRUE(ask<void>([&](auto d) { store().add_member(new_room(), bob_, std::move(d)); }));

    std::map<std::string, RoomEntry> expected;
    expected.emplace(direct.to_string(), RoomEntry{.room = direct,
                                                   .kind = RoomKind::DirectChat,
                                                   .role = MemberRole::Member,
                                                   .peer = bob_});
    expected.emplace(mine.to_string(), RoomEntry{.room = mine,
                                                 .kind = RoomKind::GroupChat,
                                                 .role = MemberRole::Admin,
                                                 .peer = std::nullopt});
    expected.emplace(theirs.to_string(), RoomEntry{.room = theirs,
                                                   .kind = RoomKind::GroupChat,
                                                   .role = MemberRole::Member,
                                                   .peer = std::nullopt});
    expected.emplace(listed.to_string(), RoomEntry{.room = listed,
                                                   .kind = RoomKind::GroupChat,
                                                   .role = MemberRole::Member,
                                                   .peer = std::nullopt});
    std::vector<RoomEntry> paged;
    std::optional<core::RoomId> cursor;
    for (;;) {
        const auto page = rooms_of(alice_, cursor, 3);
        ASSERT_TRUE(page);
        ASSERT_LE(page->size(), 3U);
        if (page->empty()) {
            break;
        }
        paged.insert(paged.end(), page->begin(), page->end());
        cursor = page->back().room;
    }
    std::vector<RoomEntry> ordered;
    ordered.reserve(expected.size());
    for (const auto& [id, entry] : expected) {
        ordered.push_back(entry);
    }
    EXPECT_EQ(paged, ordered);
    EXPECT_EQ(rooms_of(dave_, std::nullopt, 10), std::vector<RoomEntry>{});
}

TEST_P(MembershipConformance, ARoomsMembersAreReadOnlyByOneOfThemAndPageInIdOrder) {
    const core::RoomId room = group();
    EXPECT_EQ(roster(room, dave_), (Roster{.asker_listed = false, .members = {}}));
    EXPECT_EQ(roster(named_room("04"), dave_), (Roster{.asker_listed = false, .members = {}}));
    const auto first = roster(room, carol_, std::nullopt, 2);
    ASSERT_TRUE(first);
    EXPECT_EQ(first->members,
              (std::vector<MemberEntry>{{.user = alice_, .role = MemberRole::Admin},
                                        {.user = bob_, .role = MemberRole::Member}}));
    EXPECT_EQ(
        roster(room, carol_, bob_, 2),
        (Roster{.asker_listed = true, .members = {{.user = carol_, .role = MemberRole::Member}}}));
    EXPECT_EQ(roster(room, carol_, carol_, 2), (Roster{.asker_listed = true, .members = {}}));
}

TEST_P(MembershipConformance, AUserInTheMostRoomsOpensAndCreatesNoMoreButMayBeAdded) {
    for (std::size_t i = 0; i < core::ports::kMaxRoomsPerUser; ++i) {
        const core::RoomId room = core::RoomId::generate(clock_, random_);
        ASSERT_TRUE(ask<void>([&](auto d) { store().add_member(room, dave_, std::move(d)); }));
    }
    EXPECT_EQ(open_direct(named_room("03"), dave_, bob_), refused(MembershipOutcome::RoomLimit));
    EXPECT_EQ(create_group(named_room("04"), dave_, {}), refused(MembershipOutcome::RoomLimit));
    // Others can still list him, so nobody can shut him out of new rooms.
    EXPECT_EQ(open_direct(named_room("03"), bob_, dave_), done({bob_, dave_}));
    const core::RoomId room = group();
    EXPECT_EQ(add(room, alice_, {dave_}), done({dave_}));
}

TEST_P(MembershipConformance, AGroupEveryoneLeftIsNotCreatedAgainOverItsHistory) {
    const core::RoomId room = named_room("04");
    backend_->open(room);
    ASSERT_EQ(create_group(room, alice_, {}), done({alice_}));
    ASSERT_TRUE(write(room, alice_, bytes("kept")));
    ASSERT_EQ(leave(room, alice_), done({alice_}));
    EXPECT_EQ(create_group(room, alice_, {bob_}), refused(MembershipOutcome::Gone));
    EXPECT_EQ(members(room, std::nullopt, 10), std::vector<core::UserId>{});
}

// The operator's backend, through chat's service API, acts as every group's admin without being
// on its list, under the same lock, kinds and cap as a user's change.
TEST_P(MembershipConformance, TheServiceAddsToAnyGroupAndReadsAnyListWithoutBeingOnIt) {
    const core::ports::Actor service = core::ports::Actor::service();
    const core::RoomId room = group();
    EXPECT_EQ(add(room, service, {dave_, bob_}), done({dave_}));
    EXPECT_EQ(add(room, service, {dave_}), done({}));
    EXPECT_EQ(roster(room, service),
              (Roster{.asker_listed = true,
                      .members = {{.user = alice_, .role = MemberRole::Admin},
                                  {.user = bob_, .role = MemberRole::Member},
                                  {.user = carol_, .role = MemberRole::Member},
                                  {.user = dave_, .role = MemberRole::Member}}}));
    EXPECT_EQ(
        roster(room, service, bob_, 1),
        (Roster{.asker_listed = true, .members = {{.user = carol_, .role = MemberRole::Member}}}));
    // A room nothing recorded is nobody's to add to, and lists nobody.
    const core::RoomId unknown = named_room("04");
    EXPECT_EQ(add(unknown, service, {dave_}), refused(MembershipOutcome::NotMember));
    EXPECT_EQ(roster(unknown, service), (Roster{.asker_listed = true, .members = {}}));
    // A direct chat's pair never changes, whoever asks.
    const core::RoomId direct = named_room("03");
    ASSERT_EQ(open_direct(direct, alice_, bob_), done({alice_, bob_}));
    EXPECT_EQ(add(direct, service, {carol_}), refused(MembershipOutcome::NotGroup));
    // The cap holds for the service too.
    std::size_t next = 0;
    const auto fill = [&](std::size_t n) {
        std::vector<core::UserId> batch;
        batch.reserve(n);
        for (std::size_t i = 0; i < n; ++i) {
            batch.push_back(user(std::format("s{:03}", next++)));
        }
        return batch;
    };
    // Four listed: alice, bob, carol and dave.
    ASSERT_EQ(add(room, service, fill(50))->changed.size(), 50U);
    ASSERT_EQ(add(room, service, fill(core::ports::kMaxGroupMembers - 54))->changed.size(),
              core::ports::kMaxGroupMembers - 54);
    EXPECT_EQ(members(room, std::nullopt, 1000)->size(), core::ports::kMaxGroupMembers);
    EXPECT_EQ(add(room, service, {user("one-more")}), refused(MembershipOutcome::Full));
}

// The service lists people only in a group someone created (ADR-0096): not in a room nothing
// recorded, nor in one a refused join recorded that lists nobody, nor over an emptied group's
// history.
TEST_P(MembershipConformance, TheServiceAddsOnlyToAGroupSomeoneCreated) {
    const core::ports::Actor service = core::ports::Actor::service();
    EXPECT_EQ(add(named_room("04"), service, {dave_}), refused(MembershipOutcome::NotMember));
    const core::RoomId joined = named_room("04");
    ASSERT_EQ(ask<Admission>([&](auto d) {
                  store().admits(joined, carol_, RoomKind::GroupChat, std::move(d));
              }),
              Admission::NotMember);
    EXPECT_EQ(add(joined, service, {dave_}), refused(MembershipOutcome::NotMember));
    EXPECT_EQ(members(joined, std::nullopt, 10), std::vector<core::UserId>{});
    const core::RoomId emptied = named_room("04");
    backend_->open(emptied);
    ASSERT_EQ(create_group(emptied, alice_, {}), done({alice_}));
    ASSERT_TRUE(write(emptied, alice_, bytes("kept")));
    ASSERT_EQ(leave(emptied, alice_), done({alice_}));
    EXPECT_EQ(add(emptied, service, {bob_}), refused(MembershipOutcome::Gone));
    EXPECT_EQ(members(emptied, std::nullopt, 10), std::vector<core::UserId>{});
}

// The operator's backend takes a direct chat apart (an unfriend, a block): both unlisted, each
// removal told, and a later open lists the pair again.
TEST_P(MembershipConformance, AClosedDirectChatListsNobodyUntilOpenedAgain) {
    Heard heard;
    ASSERT_NO_FATAL_FAILURE(backend_->watch(heard, heard.resyncs));
    const auto close = [&](const core::RoomId& room) {
        return ask<MembershipChange>(
            [&](auto done) { store().close_direct(room, std::move(done)); });
    };
    const core::RoomId direct = named_room("03");
    ASSERT_EQ(open_direct(direct, alice_, bob_), done({alice_, bob_}));
    EXPECT_EQ(close(direct), done({alice_, bob_}));
    EXPECT_EQ(close(direct), done({}));
    EXPECT_EQ(members(direct, std::nullopt, 10), std::vector<core::UserId>{});
    EXPECT_EQ(shared(alice_, {bob_}), std::vector<core::UserId>{});
    EXPECT_EQ(ask<Admission>([&](auto d) {
                  store().admits(direct, alice_, RoomKind::DirectChat, std::move(d));
              }),
              Admission::NotMember);
    EXPECT_EQ(close(named_room("03")), done({}));
    EXPECT_EQ(close(group()), refused(MembershipOutcome::NotGroup));
    EXPECT_EQ(open_direct(direct, bob_, alice_), done({alice_, bob_}));
    const std::string r = direct.to_string();
    const auto told = [&](const std::string& change) {
        return std::ranges::find(heard.changes, change) != heard.changes.end();
    };
    EXPECT_TRUE(ulw::test::pump_until(backend_->reactor(), [&] {
        return told("- " + r + " auth0|alice") && told("- " + r + " auth0|bob");
    }));
    store().watch_members(nullptr);
}

// Whose presence a user may see (ADR-0096): those they share a direct or group chat with now.
TEST_P(MembershipConformance, SharedWithAnswersWhoShareADirectOrGroupChatNow) {
    const core::RoomId direct = named_room("03");
    ASSERT_EQ(open_direct(direct, alice_, bob_), done({alice_, bob_}));
    const core::RoomId room = named_room("04");
    ASSERT_EQ(create_group(room, alice_, {carol_}), done({alice_, carol_}));
    // Listed by an operator in a room nothing else recorded: a group chat, which counts.
    const core::RoomId listed = core::RoomId::generate(clock_, random_);
    ASSERT_TRUE(ask<void>([&](auto d) { store().add_member(listed, bob_, std::move(d)); }));
    ASSERT_TRUE(ask<void>([&](auto d) { store().add_member(listed, dave_, std::move(d)); }));
    const auto none = std::vector<core::UserId>{};
    EXPECT_EQ(shared(alice_, {dave_, carol_, alice_, bob_, carol_}),
              (std::vector<core::UserId>{bob_, carol_}));
    EXPECT_EQ(shared(alice_, {dave_}), none);
    EXPECT_EQ(shared(bob_, {dave_, carol_}), std::vector<core::UserId>{dave_});
    EXPECT_EQ(shared(dave_, {alice_}), none);
    EXPECT_EQ(shared(alice_, {}), none);
    // Leaving the only room shared ends it at once.
    ASSERT_EQ(leave(room, carol_), done({carol_}));
    EXPECT_EQ(shared(alice_, {carol_, bob_}), std::vector<core::UserId>{bob_});
    EXPECT_EQ(shared(carol_, {alice_}), none);
    ASSERT_TRUE(ask<void>([&](auto d) { store().remove_member(direct, bob_, std::move(d)); }));
    EXPECT_EQ(shared(alice_, {bob_}), none);
}

// A caller asking for a full page and one more learns whether another page follows.
TEST_P(MembershipConformance, ListingsAnswerOneEntryMoreThanAPage) {
    const core::RoomId crowded = core::RoomId::generate(clock_, random_);
    ASSERT_TRUE(ask<void>([&](auto d) { store().add_member(crowded, alice_, std::move(d)); }));
    for (std::size_t i = 0; i < core::ports::kMaxListPage + 1; ++i) {
        const core::RoomId room = core::RoomId::generate(clock_, random_);
        ASSERT_TRUE(ask<void>([&](auto d) { store().add_member(room, alice_, std::move(d)); }));
        const core::UserId member = user(std::format("u{:03}", i));
        ASSERT_TRUE(ask<void>([&](auto d) { store().add_member(crowded, member, std::move(d)); }));
    }
    EXPECT_EQ(rooms_of(alice_, std::nullopt, 1000)->size(), core::ports::kMaxListPage + 1);
    EXPECT_EQ(roster(crowded, alice_, std::nullopt, 1000)->members.size(),
              core::ports::kMaxListPage + 1);
}

// Every node hears every change, whoever made it: the listener is told of each user a change
// listed or took off, and of nobody a refused or repeated change named.
TEST_P(MembershipConformance, EveryChangeIsToldToTheListenerUserByUser) {
    Heard heard;
    ASSERT_NO_FATAL_FAILURE(backend_->watch(heard, heard.resyncs));
    const core::RoomId direct = named_room("03");
    ASSERT_EQ(open_direct(direct, alice_, bob_), done({alice_, bob_}));
    ASSERT_EQ(open_direct(direct, bob_, alice_), done({}));
    const core::RoomId room = named_room("04");
    ASSERT_EQ(create_group(room, alice_, {bob_}), done({alice_, bob_}));
    ASSERT_EQ(add(room, bob_, {carol_}), refused(MembershipOutcome::NotAdmin));
    ASSERT_EQ(add(room, alice_, {carol_}), done({carol_}));
    ASSERT_EQ(expel(room, alice_, carol_), done({carol_}));
    ASSERT_EQ(leave(room, alice_), done({alice_}, bob_));
    const std::vector<std::string> expected{
        "+ " + direct.to_string() + " auth0|alice", "+ " + direct.to_string() + " auth0|bob",
        "+ " + room.to_string() + " auth0|alice",   "+ " + room.to_string() + " auth0|bob",
        "+ " + room.to_string() + " auth0|carol",   "- " + room.to_string() + " auth0|carol",
        "- " + room.to_string() + " auth0|alice",   "* " + room.to_string() + " admin auth0|bob"};
    ASSERT_TRUE(ulw::test::pump_until(backend_->reactor(),
                                      [&] { return heard.changes.size() >= expected.size(); }));
    ulw::test::pump_pending(backend_->reactor());
    // Within one change the order of its users is the store's; across changes, commit order.
    auto sorted = [](std::vector<std::string> v, std::size_t from, std::size_t to) {
        std::sort(v.begin() + static_cast<std::ptrdiff_t>(from),
                  v.begin() + static_cast<std::ptrdiff_t>(to));
        return v;
    };
    EXPECT_EQ(sorted(sorted(heard.changes, 0, 2), 2, 4), expected);
    store().watch_members(nullptr);
}

// The in-memory store's own writer, which the Postgres store does
// not have.
class MemoryMessageStoreAppend : public ::testing::Test {
protected:
    MemoryMessageStoreAppend() {
        auto reactor = net::make_reactor(net::ReactorKind::Epoll, clock_, 1024);
        EXPECT_TRUE(reactor);
        reactor_ = std::move(*reactor);
        store_.emplace(*reactor_);
    }

    MessageResult<std::uint64_t> append(std::uint64_t seq, const core::UserId& sender,
                                        std::string key, std::vector<std::byte> body,
                                        core::WallTime sent_at) {
        return ulw::test::ask<std::uint64_t>(*reactor_, [&](auto done) {
            store_->append(room_, seq, sender, std::move(key), std::move(body), sent_at,
                           std::move(done));
        });
    }

    Page all() {
        return ulw::test::ask<Page>(*reactor_,
                                    [&](auto done) {
                                        store_->history_after(room_, 0, kMaxHistoryRows,
                                                              std::move(done));
                                    })
            .value_or(Page{});
    }

    os::SystemClock clock_;
    os::SystemRandom random_;
    std::unique_ptr<net::IReactor> reactor_;
    std::optional<infra::messages::MemoryMessageStore> store_;
    const core::RoomId room_ = core::RoomId::generate(clock_, random_);
    const core::UserId alice_ = user("auth0|alice");
    const core::UserId bob_ = user("auth0|bob");
};

TEST_F(MemoryMessageStoreAppend, RepeatingAnAppendSucceedsAndKeepsTheFirstSentAt) {
    ASSERT_TRUE(append(1, alice_, "k1", bytes("hello"), at(1)));
    EXPECT_TRUE(append(1, alice_, "k1", bytes("hello"), at(9)));
    const Page page = all();
    ASSERT_EQ(page.size(), 1U);
    EXPECT_EQ(page.front().sent_at, at(1));
}

TEST_F(MemoryMessageStoreAppend, OtherBytesAnotherSenderOrKeyUnderAStoredSeqConflict) {
    const auto conflict =
        MessageResult<std::uint64_t>{std::unexpected(MessageStoreError::Conflict)};
    ASSERT_TRUE(append(1, alice_, "k1", bytes("hello"), at(1)));
    EXPECT_EQ(append(1, alice_, "k1", bytes("hellp"), at(1)), conflict);
    EXPECT_EQ(append(1, alice_, "k1", bytes("hello!"), at(1)), conflict);
    EXPECT_EQ(append(1, bob_, "k1", bytes("hello"), at(1)), conflict);
    EXPECT_EQ(append(1, alice_, "other", bytes("hello"), at(1)), conflict);
    const Page page = all();
    ASSERT_EQ(page.size(), 1U);
    EXPECT_EQ(page.front().body, bytes("hello"));
}

TEST_F(MemoryMessageStoreAppend, AKeyUsedAgainIsAnsweredWithItsSeqAndTakesNoOther) {
    ASSERT_EQ(append(1, alice_, "k1", bytes("hello"), at(1)), 1U);
    EXPECT_EQ(append(2, alice_, "k1", bytes("hello"), at(1)), 1U);
    // Keys are per sender.
    EXPECT_EQ(append(2, bob_, "k1", bytes("hello"), at(1)), 2U);
    EXPECT_EQ(all().size(), 2U);
}

TEST_F(MemoryMessageStoreAppend, AGapInSeqIsSkippedNotFilled) {
    ASSERT_TRUE(append(1, alice_, "k1", bytes("a"), at(0)));
    ASSERT_TRUE(append(4, alice_, "k4", bytes("b"), at(0)));
    const Page page = all();
    ASSERT_EQ(page.size(), 2U);
    EXPECT_EQ(page[1].seq, 4U);
}

TEST_F(MemoryMessageStoreAppend, ABodyOverTheBoundIsRefusedAndNothingIsStored) {
    EXPECT_EQ(append(1, alice_, "k1", std::vector<std::byte>(kMaxMessageBody + 1), at(0)),
              MessageResult<std::uint64_t>{std::unexpected(MessageStoreError::TooLarge)});
    EXPECT_TRUE(all().empty());
}

std::vector<BackendFactory> backends() {
    return {
        {.name = "Memory", .make = [] { return std::make_unique<MemoryBackend>(); }},
        {.name = "Postgres", .make = &PostgresBackend::make},
    };
}

INSTANTIATE_TEST_SUITE_P(Backends, MessageStoreConformance, ::testing::ValuesIn(backends()),
                         [](const auto& param) { return param.param.name; });
INSTANTIATE_TEST_SUITE_P(Backends, MembershipConformance, ::testing::ValuesIn(backends()),
                         [](const auto& param) { return param.param.name; });

} // namespace
