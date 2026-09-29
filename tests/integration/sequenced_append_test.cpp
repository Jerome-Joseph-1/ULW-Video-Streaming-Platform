// The owner's write path for a durable room: the fenced seq and the message's row, taken
// together by PgRoomStore::append_message, and read back through the message store.
#include "infra/postgres/message_store.hpp"
#include "infra/postgres/room_store.hpp"
#include "net/offload_pool.hpp"
#include "net/reactor_factory.hpp"
#include "os/system_clock.hpp"
#include "os/system_random.hpp"

#include "conformance/message_store_harness.hpp"
#include "postgres_harness.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <format>
#include <gtest/gtest.h>
#include <memory>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using core::ports::StoredMessage;
using infra::postgres::Params;
using rt::StoreResult;
using ulw::test::scalar;
using ulw::test::ScratchDatabase;
using Seq = core::ports::MessageResult<std::optional<std::uint64_t>>;

std::vector<std::byte> bytes(std::string_view text) {
    std::vector<std::byte> out(text.size());
    std::ranges::transform(text, out.begin(), [](char c) { return static_cast<std::byte>(c); });
    return out;
}

double millis(std::chrono::steady_clock::duration d) {
    return std::chrono::duration<double, std::milli>(d).count();
}

class SequencedAppendTest : public ::testing::Test {
protected:
    void SetUp() override {
        ScratchDatabase::open(db_);
        if (IsSkipped() || HasFatalFailure()) {
            return;
        }
        conn_.emplace(db_->session());
        auto reactor = net::make_reactor(net::ReactorKind::Epoll, clock_, 1024);
        ASSERT_TRUE(reactor);
        reactor_ = std::move(*reactor);
        auto offload = net::OffloadPool::create(*reactor_, 1);
        ASSERT_TRUE(offload);
        offload_ = std::move(*offload);
        auto rooms = infra::postgres::PgRoomStore::create(*reactor_, *offload_,
                                                          {.conninfo = db_->conninfo()});
        ASSERT_TRUE(rooms) << rooms.error();
        rooms_ = std::move(*rooms);
        auto messages = infra::postgres::PgMessageStore::create(*reactor_, *offload_,
                                                                {.conninfo = db_->conninfo()});
        ASSERT_TRUE(messages) << messages.error();
        messages_ = std::move(*messages);
    }

    void TearDown() override {
        offload_.reset();
        messages_.reset();
        rooms_.reset();
        reactor_.reset();
    }

    template <class T, class Call> StoreResult<T> ask(Call call) {
        std::optional<StoreResult<T>> answer;
        call([&answer](StoreResult<T> r) noexcept { answer = std::move(r); });
        if (!ulw::test::pump_until(*reactor_, [&] { return answer.has_value(); })) {
            ADD_FAILURE() << "the room store never answered";
            return std::unexpected(rt::StoreError::Unavailable);
        }
        return std::move(*answer);
    }

    std::uint64_t owned_by(const core::RoomId& room, const core::NodeId& node) {
        const auto owner =
            ask<rt::Ownership>([&](auto done) { rooms_->resolve(room, node, std::move(done)); });
        EXPECT_TRUE(owner);
        EXPECT_EQ(owner ? owner->node : a_, node);
        return owner ? owner->generation : 0;
    }

    // A message under a key of its own.
    Seq send(const core::RoomId& room, std::uint64_t generation, std::string_view body) {
        return send(room, generation, body, std::format("m{}", ++keys_));
    }

    Seq send(const core::RoomId& room, std::uint64_t generation, std::string_view body,
             std::string_view key) {
        return write(room, generation, bytes(body), key);
    }

    Seq write(const core::RoomId& room, std::uint64_t generation, std::span<const std::byte> body,
              std::string_view key) {
        return ulw::test::ask<std::optional<std::uint64_t>>(*reactor_, [&](auto done) {
            rooms_->append_message(room, generation, alice_, key, body, std::move(done));
        });
    }

    std::vector<StoredMessage> history(const core::RoomId& room) {
        const auto page = ulw::test::ask<std::vector<StoredMessage>>(*reactor_, [&](auto done) {
            messages_->history_after(room, 0, core::ports::kMaxHistoryRows, std::move(done));
        });
        EXPECT_TRUE(page);
        return page.value_or(std::vector<StoredMessage>{});
    }

    std::string last_seq(const core::RoomId& room) {
        return scalar(*conn_, "SELECT last_seq FROM room_state WHERE room_id = $1",
                      Params{}.add_uuid(room.uuid()));
    }

    void go_quiet(const core::RoomId& room) {
        ASSERT_TRUE(
            conn_->exec("UPDATE room_assignments SET heartbeat_at = now() - interval '6 seconds' "
                        "WHERE room_id = $1",
                        Params{}.add_uuid(room.uuid())));
    }

    core::RoomId new_room() { return core::RoomId::generate(clock_, random_); }

    os::SystemClock clock_;
    os::SystemRandom random_;
    std::unique_ptr<ScratchDatabase> db_;
    std::optional<infra::postgres::SyncConnection> conn_;
    std::unique_ptr<net::IReactor> reactor_;
    std::unique_ptr<net::OffloadPool> offload_;
    std::unique_ptr<infra::postgres::PgRoomStore> rooms_;
    std::unique_ptr<infra::postgres::PgMessageStore> messages_;
    const core::NodeId a_ = *core::NodeId::parse("chat-a");
    const core::NodeId b_ = *core::NodeId::parse("chat-b");
    const core::UserId alice_ = *core::UserId::parse("auth0|alice");
    std::uint64_t keys_ = 0;
};

TEST_F(SequencedAppendTest, EachMessageIsStoredUnderTheSeqItTook) {
    const core::RoomId room = new_room();
    const std::uint64_t generation = owned_by(room, a_);
    EXPECT_EQ(send(room, generation, "one"), Seq{1});
    EXPECT_EQ(send(room, generation, "two"), Seq{2});
    EXPECT_EQ(send(room, generation, "three"), Seq{3});
    EXPECT_EQ(last_seq(room), "3");

    const auto stored = history(room);
    ASSERT_EQ(stored.size(), 3U);
    EXPECT_EQ(stored[0].seq, 1U);
    EXPECT_EQ(stored[0].sender, alice_);
    EXPECT_EQ(stored[0].key, "m1");
    EXPECT_EQ(stored[0].body, bytes("one"));
    // The database's clock at the write, not one the owner passed.
    EXPECT_LE(stored[0].sent_at, stored[2].sent_at);
    EXPECT_EQ(stored[2].seq, 3U);
    EXPECT_EQ(stored[2].body, bytes("three"));
}

TEST_F(SequencedAppendTest, AFencedWriterTakesNoSeqAndStoresNoRow) {
    const core::RoomId room = new_room();
    const std::uint64_t first = owned_by(room, a_);
    ASSERT_EQ(send(room, first, "before the takeover"), Seq{1});
    go_quiet(room);
    const std::uint64_t second = owned_by(room, b_);
    ASSERT_GT(second, first);

    EXPECT_EQ(send(room, first, "from the former owner"), Seq{std::nullopt});
    EXPECT_EQ(last_seq(room), "1");
    EXPECT_EQ(history(room).size(), 1U);

    EXPECT_EQ(send(room, second, "from the new owner"), Seq{2});
    const auto stored = history(room);
    ASSERT_EQ(stored.size(), 2U);
    EXPECT_EQ(stored[1].body, bytes("from the new owner"));
}

TEST_F(SequencedAppendTest, ARepeatAfterALostAnswerGetsTheOriginalSeqAndStoresNothing) {
    const core::RoomId room = new_room();
    const std::uint64_t generation = owned_by(room, a_);
    ASSERT_EQ(send(room, generation, "hello", "k1"), Seq{1});
    ASSERT_EQ(send(room, generation, "later", "k2"), Seq{2});
    // The first answer was lost; the client sends the same message again.
    EXPECT_EQ(send(room, generation, "hello", "k1"), Seq{1});
    EXPECT_EQ(last_seq(room), "2");
    const auto stored = history(room);
    ASSERT_EQ(stored.size(), 2U);
    EXPECT_EQ(stored[0].key, "k1");
    EXPECT_EQ(stored[1].key, "k2");
}

TEST_F(SequencedAppendTest, ARepeatThroughTheNextOwnerGetsTheOriginalSeq) {
    const core::RoomId room = new_room();
    const std::uint64_t first = owned_by(room, a_);
    ASSERT_EQ(send(room, first, "hello", "k1"), Seq{1});
    go_quiet(room);
    const std::uint64_t second = owned_by(room, b_);

    EXPECT_EQ(send(room, second, "hello", "k1"), Seq{1});
    // The former owner's repeat is fenced like any of its writes: no answer to deliver on.
    EXPECT_EQ(send(room, first, "hello", "k1"), Seq{std::nullopt});
    EXPECT_EQ(last_seq(room), "1");
    EXPECT_EQ(history(room).size(), 1U);
}

TEST_F(SequencedAppendTest, ConcurrentWritesToOneRoomTakeEverySeqOnce) {
    // More in flight than the pool has sessions, so that writes queue on the room's row.
    constexpr std::size_t kWrites = 200;
    const core::RoomId room = new_room();
    const std::uint64_t generation = owned_by(room, a_);
    std::vector<Seq> answers;
    for (std::size_t i = 0; i < kWrites; ++i) {
        rooms_->append_message(room, generation, alice_, std::format("k{}", i),
                               bytes(std::format("body {}", i)),
                               [&answers](Seq r) noexcept { answers.push_back(r); });
    }
    ASSERT_TRUE(ulw::test::pump_until(*reactor_, [&] { return answers.size() == kWrites; }));
    std::vector<std::uint64_t> seqs;
    for (const Seq& answer : answers) {
        ASSERT_TRUE(answer && *answer);
        seqs.push_back(**answer);
    }
    std::ranges::sort(seqs);
    for (std::size_t i = 0; i < kWrites; ++i) {
        EXPECT_EQ(seqs[i], i + 1);
    }
    EXPECT_EQ(last_seq(room), std::to_string(kWrites));
    const auto stored = history(room);
    ASSERT_EQ(stored.size(), kWrites);
    // Each seq holds the body that was answered with it.
    for (std::size_t i = 0; i < kWrites; ++i) {
        EXPECT_EQ(stored[i].body, bytes(std::format("body {}", stored[i].key.substr(1))));
    }
}

TEST_F(SequencedAppendTest, ConcurrentRepeatsOfOneKeyStoreItOnce) {
    constexpr std::size_t kRepeats = 8;
    const core::RoomId room = new_room();
    const std::uint64_t generation = owned_by(room, a_);
    std::vector<Seq> answers;
    for (std::size_t i = 0; i < kRepeats; ++i) {
        rooms_->append_message(room, generation, alice_, "k1", bytes("hello"),
                               [&answers](Seq r) noexcept { answers.push_back(r); });
    }
    ASSERT_TRUE(ulw::test::pump_until(*reactor_, [&] { return answers.size() == kRepeats; }));
    for (const Seq& answer : answers) {
        EXPECT_EQ(answer, Seq{1});
    }
    EXPECT_EQ(last_seq(room), "1");
    EXPECT_EQ(history(room).size(), 1U);
}

TEST_F(SequencedAppendTest, AnOversizedBodyOrKeyIsTooLargeAndTakesNoSeq) {
    const core::RoomId room = new_room();
    const std::uint64_t generation = owned_by(room, a_);
    const Seq too_large{std::unexpected(core::ports::MessageStoreError::TooLarge)};
    // What a peer node's frame can carry past the client edge's bound.
    const std::vector<std::byte> body(core::ports::kMaxMessageBody + 1);
    EXPECT_EQ(write(room, generation, body, "k1"), too_large);
    EXPECT_EQ(
        write(room, generation, bytes("hello"), std::string(core::ports::kMaxMessageKey + 1, 'k')),
        too_large);
    EXPECT_EQ(last_seq(room), "0");
    EXPECT_EQ(send(room, generation, "hello"), Seq{1});
}

TEST_F(SequencedAppendTest, AnInsertThatFailsGivesTheSeqBack) {
    const core::RoomId room = new_room();
    const std::uint64_t generation = owned_by(room, a_);
    // A row where the next seq would go, which nothing but a broken writer leaves.
    ASSERT_TRUE(
        conn_->exec("INSERT INTO chat_messages (room_id, seq, sender, msg_key, body, sent_at) "
                    "VALUES ($1, 1, 'auth0|mallory', 'k', '\\x00', now())",
                    Params{}.add_uuid(room.uuid())));
    EXPECT_EQ(send(room, generation, "lost"),
              Seq{std::unexpected(core::ports::MessageStoreError::Unavailable)});
    // The seq was not taken without its row.
    EXPECT_EQ(last_seq(room), "0");
}

TEST_F(SequencedAppendTest, StoringTheMessageCostsWhatTakingTheSeqAloneDoes) {
    // An owner writes a room's messages one at a time (ADR-0035), each before delivery: these
    // are per-message latencies of a durable room. Reported, not asserted: they measure the
    // host as much as the statement.
    constexpr std::size_t kCount = 300;
    const core::RoomId with_row = new_room();
    const core::RoomId seq_only = new_room();
    const std::uint64_t g_with_row = owned_by(with_row, a_);
    const std::uint64_t g_seq_only = owned_by(seq_only, a_);
    std::vector<double> combined;
    std::vector<double> alone;
    for (std::size_t i = 0; i < kCount; ++i) {
        auto started = std::chrono::steady_clock::now();
        ASSERT_TRUE(send(with_row, g_with_row, "a line of chat, about forty bytes long"));
        combined.push_back(millis(std::chrono::steady_clock::now() - started));

        started = std::chrono::steady_clock::now();
        ASSERT_TRUE(ask<std::optional<std::uint64_t>>([&](auto done) {
            rooms_->append(
                seq_only, g_seq_only,
                rt::Outgoing{.sender = alice_, .key = *rt::MessageKey::parse("k"), .body = {}},
                std::move(done));
        }));
        alone.push_back(millis(std::chrono::steady_clock::now() - started));
    }
    const auto report = [&](std::string_view name, std::vector<double>& samples) {
        std::ranges::sort(samples);
        const auto at_quantile = [&](double q) {
            return samples[static_cast<std::size_t>(q * static_cast<double>(samples.size() - 1))];
        };
        std::println("{}: p50 {:.3f} ms, p99 {:.3f} ms", name, at_quantile(0.5), at_quantile(0.99));
        RecordProperty(std::format("{}_p50_ms", name), std::format("{:.3f}", at_quantile(0.5)));
        RecordProperty(std::format("{}_p99_ms", name), std::format("{:.3f}", at_quantile(0.99)));
    };
    report("seq_and_row_in_one_statement", combined);
    report("seq_alone", alone);
}

} // namespace
