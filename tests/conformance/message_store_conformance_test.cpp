// Laws every IMessageStore must obey, run against each store.
#include "infra/messages/memory_message_store.hpp"
#include "net/reactor_factory.hpp"
#include "os/system_clock.hpp"
#include "os/system_random.hpp"

#include "message_store_harness.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <functional>
#include <gtest/gtest.h>
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

private:
    std::optional<infra::messages::MemoryMessageStore> store_;
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

    MessageResult<void> append(const core::RoomId& room, std::uint64_t seq,
                               const core::UserId& sender, std::vector<std::byte> body,
                               core::WallTime sent_at) {
        return ask<void>([&](auto done) {
            store().append(room, seq, sender, std::move(body), sent_at, std::move(done));
        });
    }

    // `count` messages, 1..count, several in flight at a time as an owner's retries or many
    // rooms would put them.
    void append_many(const core::RoomId& room, std::uint64_t count) {
        constexpr std::uint64_t kWindow = 256;
        for (std::uint64_t from = 1; from <= count; from += kWindow) {
            const std::uint64_t to = std::min(count, from + kWindow - 1);
            std::uint64_t answered = 0;
            std::uint64_t failed = 0;
            for (std::uint64_t seq = from; seq <= to; ++seq) {
                store().append(room, seq, alice_, body_of(seq), at(0),
                               [&](MessageResult<void> r) noexcept {
                                   ++answered;
                                   failed += r ? 0U : 1U;
                               });
            }
            ASSERT_TRUE(ulw::test::pump_until(backend_->reactor(),
                                              [&] { return answered == to - from + 1; }));
            ASSERT_EQ(failed, 0U);
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

    core::RoomId new_room() { return core::RoomId::generate(clock_, random_); }

    os::SystemClock clock_;
    os::SystemRandom random_;
    std::unique_ptr<Backend> backend_;
    const core::UserId alice_ = user("auth0|alice");
    const core::UserId bob_ = user("auth0|bob");
};

TEST_P(MessageStoreConformance, AMessageComesBackWithEveryByteItWasStoredWith) {
    const core::RoomId room = new_room();
    std::vector<std::byte> every(256);
    for (std::size_t i = 0; i < every.size(); ++i) {
        every[i] = static_cast<std::byte>(i);
    }
    ASSERT_TRUE(append(room, 1, alice_, every, at(1)));
    ASSERT_TRUE(append(room, 2, bob_, {}, at(2)));

    const auto page = before(room, std::nullopt, 10);
    ASSERT_TRUE(page);
    EXPECT_EQ(*page, (Page{{.seq = 2, .sender = bob_, .sent_at = at(2), .body = {}},
                           {.seq = 1, .sender = alice_, .sent_at = at(1), .body = every}}));
}

TEST_P(MessageStoreConformance, SentAtKeepsMicrosecondsAndDropsWhatIsFiner) {
    const core::RoomId room = new_room();
    const core::WallTime sent =
        at(0) + std::chrono::microseconds{123'456} +
        std::chrono::duration_cast<core::WallTime::duration>(std::chrono::nanoseconds{789});
    ASSERT_TRUE(append(room, 1, alice_, bytes("x"), sent));
    const auto page = before(room, std::nullopt, 1);
    ASSERT_TRUE(page);
    ASSERT_EQ(page->size(), 1U);
    EXPECT_EQ(page->front().sent_at, at(0) + std::chrono::microseconds{123'456});
}

TEST_P(MessageStoreConformance, HistoryBeforeGoesBackFromTheCursorNewestFirst) {
    const core::RoomId room = new_room();
    ASSERT_NO_FATAL_FAILURE(append_many(room, 10));
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
    ASSERT_NO_FATAL_FAILURE(append_many(room, 10));
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

TEST_P(MessageStoreConformance, AGapInSeqIsSkippedNotFilled) {
    const core::RoomId room = new_room();
    ASSERT_TRUE(append(room, 1, alice_, bytes("a"), at(0)));
    ASSERT_TRUE(append(room, 4, alice_, bytes("b"), at(0)));
    const auto page = after(room, 0, 10);
    ASSERT_TRUE(page);
    EXPECT_EQ(seqs(*page), (std::vector<std::uint64_t>{1, 4}));
    EXPECT_EQ(last_seq(room), 4U);
}

TEST_P(MessageStoreConformance, AZeroLimitIsAnEmptyPage) {
    const core::RoomId room = new_room();
    ASSERT_TRUE(append(room, 1, alice_, bytes("a"), at(0)));
    EXPECT_EQ(before(room, std::nullopt, 0), Page{});
    EXPECT_EQ(after(room, 0, 0), Page{});
}

TEST_P(MessageStoreConformance, AskingForMoreThanTheRowCapGetsTheCap) {
    const core::RoomId room = new_room();
    ASSERT_NO_FATAL_FAILURE(append_many(room, kMaxHistoryRows + 10));
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
    // Four largest bodies fill the bound exactly; the fifth goes to the next page.
    static_assert(4 * kMaxMessageBody == kMaxHistoryBytes);
    for (std::uint64_t seq = 1; seq <= 5; ++seq) {
        std::vector<std::byte> body(kMaxMessageBody, static_cast<std::byte>(seq));
        ASSERT_TRUE(append(room, seq, alice_, std::move(body), at(0)));
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

TEST_P(MessageStoreConformance, ABodyOverTheBoundIsRefusedAndNothingIsStored) {
    const core::RoomId room = new_room();
    EXPECT_EQ(append(room, 1, alice_, std::vector<std::byte>(kMaxMessageBody + 1), at(0)),
              MessageResult<void>{std::unexpected(MessageStoreError::TooLarge)});
    EXPECT_EQ(last_seq(room), 0U);
    EXPECT_TRUE(append(room, 1, alice_, std::vector<std::byte>(kMaxMessageBody), at(0)));
}

TEST_P(MessageStoreConformance, RepeatingAnAppendSucceedsAndKeepsTheFirstSentAt) {
    const core::RoomId room = new_room();
    ASSERT_TRUE(append(room, 1, alice_, bytes("hello"), at(1)));
    EXPECT_TRUE(append(room, 1, alice_, bytes("hello"), at(9)));
    const auto page = after(room, 0, 10);
    ASSERT_TRUE(page);
    ASSERT_EQ(page->size(), 1U);
    EXPECT_EQ(page->front().sent_at, at(1));
}

TEST_P(MessageStoreConformance, OtherBytesOrAnotherSenderUnderAStoredSeqConflict) {
    const core::RoomId room = new_room();
    const auto conflict = MessageResult<void>{std::unexpected(MessageStoreError::Conflict)};
    ASSERT_TRUE(append(room, 1, alice_, bytes("hello"), at(1)));
    EXPECT_EQ(append(room, 1, alice_, bytes("hellp"), at(1)), conflict);
    EXPECT_EQ(append(room, 1, alice_, bytes("hello!"), at(1)), conflict);
    EXPECT_EQ(append(room, 1, bob_, bytes("hello"), at(1)), conflict);
    const auto page = after(room, 0, 10);
    ASSERT_TRUE(page);
    EXPECT_EQ(*page,
              (Page{{.seq = 1, .sender = alice_, .sent_at = at(1), .body = bytes("hello")}}));
}

TEST_P(MessageStoreConformance, LastSeqIsZeroForAnEmptyRoomAndTheHighestStoredOtherwise) {
    const core::RoomId room = new_room();
    EXPECT_EQ(last_seq(room), 0U);
    ASSERT_TRUE(append(room, 2, alice_, bytes("b"), at(0)));
    ASSERT_TRUE(append(room, 1, alice_, bytes("a"), at(0)));
    EXPECT_EQ(last_seq(room), 2U);
}

TEST_P(MessageStoreConformance, RoomsAreKeptApart) {
    const core::RoomId one = new_room();
    const core::RoomId two = new_room();
    ASSERT_TRUE(append(one, 1, alice_, bytes("in one"), at(0)));
    ASSERT_TRUE(append(two, 1, bob_, bytes("in two"), at(0)));
    ASSERT_TRUE(append(two, 2, bob_, bytes("in two again"), at(0)));
    const auto page = before(one, std::nullopt, 10);
    ASSERT_TRUE(page);
    ASSERT_EQ(page->size(), 1U);
    EXPECT_EQ(page->front().body, bytes("in one"));
    EXPECT_EQ(last_seq(one), 1U);
    EXPECT_EQ(last_seq(two), 2U);
}

TEST_P(MessageStoreConformance, TenThousandMessagesPageBothWaysWithoutGapsOrDuplicates) {
    constexpr std::uint64_t kCount = 10'000;
    // Not a divisor of kCount, so the last page in each direction is a short one.
    constexpr std::size_t kPage = 97;
    const core::RoomId room = new_room();
    ASSERT_NO_FATAL_FAILURE(append_many(room, kCount));

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
    // A linguistic collation would put "a" before "B" and ignore the punctuation.
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

std::vector<BackendFactory> backends() {
    return {
        {.name = "Memory", .make = [] { return std::make_unique<MemoryBackend>(); }},
    };
}

INSTANTIATE_TEST_SUITE_P(Backends, MessageStoreConformance, ::testing::ValuesIn(backends()),
                         [](const auto& param) { return param.param.name; });

} // namespace
