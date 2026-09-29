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

TEST_P(MessageStoreConformance, ARoomWithoutMembersAdmitsAnyoneAndOneWithMembersOnlyThem) {
    const core::RoomId room = new_room();
    const auto admits = [&](const core::UserId& user) {
        return ask<bool>([&](auto done) { store().admits(room, user, std::move(done)); });
    };
    EXPECT_EQ(admits(alice_), true);
    EXPECT_EQ(admits(bob_), true);
    ASSERT_TRUE(ask<void>([&](auto done) { store().add_member(room, alice_, std::move(done)); }));
    EXPECT_EQ(admits(alice_), true);
    EXPECT_EQ(admits(bob_), false);
    ASSERT_TRUE(
        ask<void>([&](auto done) { store().remove_member(room, alice_, std::move(done)); }));
    EXPECT_EQ(admits(bob_), true);
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

} // namespace
