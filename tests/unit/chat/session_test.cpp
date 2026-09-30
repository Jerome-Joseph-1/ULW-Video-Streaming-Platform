#include "core/util/json.hpp"
#include "core/util/parse.hpp"
#include "infra/auth/base64url.hpp"
#include "infra/messages/memory_message_store.hpp"
#include "net/reactor_factory.hpp"
#include "net/socket.hpp"
#include "os/system_clock.hpp"
#include "os/system_random.hpp"
#include "rt/room_router.hpp"

#include "chat.hpp"
#include "session.hpp"
#include "support/eventually.hpp"
#include "support/fake_clock.hpp"
#include "support/fake_random.hpp"
#include "support/fake_verifier.hpp"
#include "support/reactor_harness.hpp"
#include "support/ws_client.hpp"
#include "unit/rt/memory_room_store.hpp"

#include <atomic>
#include <cerrno>
#include <format>
#include <future>
#include <gtest/gtest.h>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using std::chrono::seconds;
using ulw::test::WsClient;

constexpr std::string_view kRoom = "01a0eb86-6cca-7dce-84cc-3bb47615f9fd";
constexpr std::string_view kAllowed = "https://app.askedin.test";

// One chat node on a thread of its own, as chat_server runs it, over an in-memory room store.
// The test talks to it through sockets only.
class Node {
public:
    // With `manual_clock`, time on the node stands still until advance() moves it.
    explicit Node(net::ReactorKind kind, chat::Limits limits = {}, bool manual_clock = false)
        : limits_(limits), manual_clock_(manual_clock) {
        std::promise<std::uint16_t> port;
        auto ready = port.get_future();
        healthy_ = healthy_promise_.get_future();
        thread_ = std::jthread([this, kind, port = std::move(port)]() mutable { run(kind, port); });
        port_ = ready.get();
    }
    ~Node() {
        stop_ = true;
        thread_.join();
    }
    Node(const Node&) = delete;
    Node& operator=(const Node&) = delete;
    Node(Node&&) = delete;
    Node& operator=(Node&&) = delete;

    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }

    // Whether the room plane is reachable, the condition /readyz reports, without a request to
    // poll it with: the node's loop says so once, and the wait ends then or at the limit.
    [[nodiscard]] bool wait_healthy(std::chrono::milliseconds limit) const {
        return healthy_.wait_for(limit) == std::future_status::ready;
    }

    // Returns once the node's loop has run the timers that came due: it applies the step before
    // polling, and the extra turns let whatever those timers did reach its sockets.
    void advance(core::Millis step) {
        const auto want = requested_.fetch_add(step.count()) + step.count();
        for (auto seen = applied_.load(); seen != want; seen = applied_.load()) {
            applied_.wait(seen);
        }
        const auto turn = turns_.load();
        for (auto seen = turns_.load(); seen < turn + 2; seen = turns_.load()) {
            turns_.wait(seen);
        }
    }
    // While set, the room store answers nothing: every send stays in flight. `store_held`
    // follows once the node's thread has seen it.
    std::atomic<bool> hold_store = false;
    std::atomic<bool> store_held = false;
    // Sessions holding an HTTP parser, as of the node's last turn.
    std::atomic<std::size_t> http_parsers = 0;
    // Upgrades waiting on the verifier's keys ("slow." tokens), as of the node's last turn;
    // setting refresh_keys lets them go on.
    std::atomic<std::size_t> key_waiters = 0;
    std::atomic<bool> refresh_keys = false;
    // Every session fails as if out of memory, and the keys arrive in the same turn, before the
    // failed sessions have closed.
    std::atomic<bool> fail_then_refresh_keys = false;

private:
    // An io_uring reactor belongs to the thread that made it, so everything is made here.
    void run(net::ReactorKind kind, std::promise<std::uint16_t>& port) {
        os::SystemClock system_clock;
        ulw::test::FakeClock fake_clock;
        core::ports::IClock& clock = manual_clock_
                                         ? static_cast<core::ports::IClock&>(fake_clock)
                                         : static_cast<core::ports::IClock&>(system_clock);
        auto reactor = net::make_reactor(kind, clock, 1024);
        auto clients = net::listen_tcp({.port = 0, .loopback_only = true});
        auto peers = net::listen_tcp({.port = 0, .loopback_only = true});
        if (!reactor || !clients || !peers) {
            port.set_value(0);
            return;
        }
        const std::uint16_t client_port = *net::local_port(clients->get());
        const std::uint16_t node_port = *net::local_port(peers->get());
        ulw::test::MemoryRooms db;
        ulw::test::FakeVerifier verifier;
        auto store = std::make_unique<ulw::test::MemoryRoomStore>(**reactor, db);
        auto messages = std::make_unique<infra::messages::MemoryMessageStore>(**reactor);
        // The rooms these tests join are group chats of the users who join them.
        for (const std::string_view room :
             {kRoom, std::string_view{"01a0eb86-6cca-7dce-84cc-3bb47615f901"},
              std::string_view{"01a0eb86-6cca-7dce-84cc-3bb47615f902"},
              std::string_view{"01a0eb86-6cca-7dce-84cc-3bb47615f903"},
              std::string_view{"01a0eb86-6cca-7dce-84cc-3bb47615f904"},
              std::string_view{"01a0eb86-6cca-7dce-84cc-3bb47615f905"}}) {
            for (const std::string_view user : {"alice", "bob", "viewer", "reader", "sender"}) {
                messages->add_member(*core::RoomId::parse(room), *core::UserId::parse(user),
                                     [](core::ports::MessageResult<void> /*added*/) noexcept {});
            }
        }
        chat::RoomLog log(*core::NodeId::parse("chat-1"));
        os::SystemRandom random;
        rt::RoomRouter router(**reactor, *store, clock, random,
                              {.self = *core::NodeId::parse("chat-1"),
                               .advertise = "127.0.0.1:" + std::to_string(node_port),
                               .secret = "session-test-node-secret-0123456789"},
                              log);
        if (!router.start(std::move(*peers))) {
            port.set_value(0);
            return;
        }
        auto server = std::make_unique<chat::ChatServer>(
            chat::Deps{.node = *core::NodeId::parse("chat-1"),
                       .reactor = **reactor,
                       .router = router,
                       .messages = *messages,
                       .verifier = verifier,
                       .clock = clock},
            chat::Access{.cookie = "auth_token", .allowed_origins = {std::string(kAllowed)}},
            limits_);
        if (!(*reactor)->listen(std::move(*clients), *server)) {
            port.set_value(0);
            return;
        }
        port.set_value(client_port);
        bool healthy_told = false;
        while (!stop_) {
            if (!healthy_told && router.healthy()) {
                healthy_told = true;
                healthy_promise_.set_value();
            }
            if (hold_store != store->hold) {
                if (hold_store) {
                    store->hold = true;
                } else {
                    store->release_held();
                }
                store_held = store->hold;
            }
            if (const auto want = requested_.load(); want != applied_.load()) {
                fake_clock.advance(core::Millis{want - applied_.load()});
                applied_ = want;
                applied_.notify_all();
            }
            (*reactor)->run_once(core::Millis{5});
            server->reap();
            if (refresh_keys.exchange(false)) {
                verifier.refresh_keys();
            }
            if (fail_then_refresh_keys.exchange(false)) {
                server->for_each_session([](chat::Session& s) noexcept { s.allocation_failed(); });
                verifier.refresh_keys();
            }
            key_waiters = verifier.waiting();
            http_parsers = server->http_parsers();
            ++turns_;
            turns_.notify_all();
        }
        messages.reset();
        server.reset();
        store.reset();
    }

    chat::Limits limits_;
    bool manual_clock_;
    std::promise<void> healthy_promise_;
    std::future<void> healthy_;
    std::atomic<std::int64_t> requested_ = 0;
    std::atomic<std::int64_t> applied_ = 0;
    std::atomic<std::uint64_t> turns_ = 0;
    std::atomic<bool> stop_ = false;
    std::uint16_t port_ = 0;
    std::jthread thread_;
};

class ChatSessionTest : public ::testing::TestWithParam<net::ReactorKind> {
protected:
    void SetUp() override {
        node_ = std::make_unique<Node>(GetParam());
        ASSERT_NE(node_->port(), 0);
    }

    std::optional<WsClient> open(const std::string& headers, std::string* refusal = nullptr) {
        return WsClient::connect(node_->port(), "/rt", headers, refusal);
    }

    std::optional<WsClient> open_as(std::string_view user) {
        return open("Authorization: Bearer user." + std::string(user) + "\r\n");
    }

    // The status line of a refused upgrade.
    std::string refusal(const std::string& headers) {
        std::string line;
        EXPECT_FALSE(open(headers, &line));
        return line;
    }

    std::unique_ptr<Node> node_;
};

TEST_P(ChatSessionTest, ProbesAnswerAndUnknownPathsAreNotFound) {
    EXPECT_EQ(ulw::test::http_get(node_->port(), "/healthz").status, 200);
    // Ready follows the node's first heartbeat, about a second on. Waiting on that instead of
    // requesting /readyz in a loop keeps the test to a handful of connections.
    ASSERT_TRUE(node_->wait_healthy(seconds(10)));
    EXPECT_EQ(ulw::test::http_get(node_->port(), "/readyz").status, 200);
    const auto metrics = ulw::test::http_get(node_->port(), "/metrics");
    EXPECT_EQ(metrics.status, 200);
    EXPECT_NE(metrics.body.find("fenced_writes_total 0\n"), std::string::npos);
    EXPECT_EQ(ulw::test::http_get(node_->port(), "/api/v1/videos").status, 404);
}

TEST_P(ChatSessionTest, AnUpgradeNeedsAValidToken) {
    EXPECT_EQ(refusal(""), "HTTP/1.1 401 Unauthorized");
    EXPECT_EQ(refusal("Authorization: Bearer forged\r\n"), "HTTP/1.1 401 Unauthorized");
    // The key server being down says nothing about the token.
    EXPECT_EQ(refusal("Authorization: Bearer down.alice\r\n"), "HTTP/1.1 503 Service Unavailable");
    EXPECT_TRUE(open_as("alice"));
}

TEST_P(ChatSessionTest, TheCookieCountsOnlyFromAnAllowedPage) {
    const std::string cookie = "Cookie: auth_token=user.alice\r\n";
    EXPECT_EQ(refusal(cookie), "HTTP/1.1 403 Forbidden");
    EXPECT_EQ(refusal(cookie + "Origin: https://evil.test\r\n"), "HTTP/1.1 403 Forbidden");
    EXPECT_TRUE(open(cookie + "Origin: " + std::string(kAllowed) + "\r\n"));
}

// Messages carry the sender the node authenticated; a node that took it from an identity header
// would let anyone speak as anyone.
TEST_P(ChatSessionTest, IdentityHeadersNeitherAuthenticateNorRenameTheSender) {
    const std::string as_alice = "X-User-Id: alice\r\nX-User-Email: alice@example.com\r\n";
    EXPECT_EQ(refusal(as_alice), "HTTP/1.1 401 Unauthorized");
    EXPECT_EQ(refusal(as_alice + "Origin: " + std::string(kAllowed) + "\r\n"),
              "HTTP/1.1 401 Unauthorized");

    auto bob = open("Authorization: Bearer user.bob\r\n" + as_alice);
    ASSERT_TRUE(bob);
    ASSERT_TRUE(bob->send_text(R"({"type":"join","room":")" + std::string(kRoom) + R"("})"));
    EXPECT_EQ(bob->next_text(seconds(10)),
              R"({"type":"joined","room":")" + std::string(kRoom) + R"(","seq":0})");
    ASSERT_TRUE(bob->send_text(R"({"type":"send","room":")" + std::string(kRoom) +
                               R"(","id":"m1","body":"eA"})"));
    EXPECT_EQ(bob->next_text(seconds(10)),
              R"({"type":"message","room":")" + std::string(kRoom) +
                  R"(","seq":1,"sender":"bob","id":"m1","body":"eA"})");
}

TEST_P(ChatSessionTest, AMemberHearsItsOwnMessageAndItsSequenceNumber) {
    auto alice = open_as("alice");
    ASSERT_TRUE(alice);
    ASSERT_TRUE(alice->send_text(R"({"type":"join","room":")" + std::string(kRoom) + R"("})"));
    EXPECT_EQ(alice->next_text(seconds(10)),
              R"({"type":"joined","room":")" + std::string(kRoom) + R"(","seq":0})");
    // base64url of `hi "there"`.
    ASSERT_TRUE(alice->send_text(R"({"type":"send","room":")" + std::string(kRoom) +
                                 R"(","id":"m5","body":"aGkgInRoZXJlIg"})"));
    EXPECT_EQ(alice->next_text(seconds(10)),
              R"({"type":"message","room":")" + std::string(kRoom) +
                  R"(","seq":1,"sender":"alice","id":"m5","body":"aGkgInRoZXJlIg"})");
    EXPECT_EQ(alice->next_text(seconds(10)),
              R"({"type":"sent","room":")" + std::string(kRoom) + R"(","id":"m5","seq":1})");
}

TEST_P(ChatSessionTest, SendingToARoomNotJoinedIsRefusedAndTheSocketStaysOpen) {
    auto alice = open_as("alice");
    ASSERT_TRUE(alice);
    ASSERT_TRUE(alice->send_text(R"({"type":"send","room":")" + std::string(kRoom) +
                                 R"(","id":"m1","body":"eA"})"));
    EXPECT_EQ(alice->next_text(seconds(10)), R"({"type":"error","reason":"not_joined","room":")" +
                                                 std::string(kRoom) + R"(","id":"m1"})");
    ASSERT_TRUE(alice->send_text("{"));
    EXPECT_EQ(alice->next_text(seconds(10)), R"({"type":"error","reason":"not_json"})");
    ASSERT_TRUE(alice->send_text(R"({"type":"join","room":"not-a-room"})"));
    EXPECT_EQ(alice->next_text(seconds(10)), R"({"type":"error","reason":"bad_room"})");
    ASSERT_TRUE(alice->send_text(R"({"type":"join","room":")" + std::string(kRoom) + R"("})"));
    EXPECT_EQ(alice->next_text(seconds(10)),
              R"({"type":"joined","room":")" + std::string(kRoom) + R"(","seq":0})");
}

TEST_P(ChatSessionTest, AReadFullOfControlFramesIsClosedAsAFlood) {
    auto alice = open_as("alice");
    ASSERT_TRUE(alice);
    std::vector<std::byte> pings;
    for (int i = 0; i < 9; ++i) {
        ASSERT_TRUE(alice->append(pings, codec::ws::Opcode::Ping, ""));
    }
    ASSERT_TRUE(alice->send_raw(pings));
    EXPECT_EQ(alice->close_status(seconds(10)), 1008);
}

TEST_P(ChatSessionTest, PingsAreAnsweredUntilTheBurstRunsOutThenTheStreamIsCut) {
    auto alice = open_as("alice");
    ASSERT_TRUE(alice);
    constexpr int kPerWrite = 4;
    std::vector<std::byte> pings;
    for (int i = 0; i < kPerWrite; ++i) {
        ASSERT_TRUE(alice->append(pings, codec::ws::Opcode::Ping, "p"));
    }
    // Each write is under the per-read cap and waits for its answers, so only the bucket, 20
    // deep and refilling at 10 a second, can stop them: after five writes, within milliseconds.
    int pongs = 0;
    std::optional<std::string> close;
    for (int write = 0; write < 10 && !close; ++write) {
        ASSERT_TRUE(alice->send_raw(pings));
        for (int i = 0; i < kPerWrite && !close; ++i) {
            const auto frame = alice->next_frame(seconds(10));
            ASSERT_TRUE(frame);
            if (frame->first == codec::ws::Opcode::Close) {
                close = frame->second;
            } else {
                ASSERT_EQ(frame->first, codec::ws::Opcode::Pong);
                EXPECT_EQ(frame->second, "p");
                ++pongs;
            }
        }
    }
    EXPECT_GE(pongs, 20);
    EXPECT_LT(pongs, 40);
    ASSERT_TRUE(close);
    ASSERT_GE(close->size(), 2U);
    EXPECT_EQ((static_cast<unsigned char>((*close)[0]) << 8U) |
                  static_cast<unsigned char>((*close)[1]),
              1008U);
}

// A socket must not outlive the token it was opened with: a user whose tokens stopped being
// issued (signed out, banned) would otherwise go on receiving for as long as it answers pings.
TEST_P(ChatSessionTest, ASocketClosesWhenItsTokenExpiresWithTheCodeThatSaysReconnect) {
    node_.reset();
    // Pings and the idle timeout far off, so that only the token can end the socket.
    node_ = std::make_unique<Node>(GetParam(),
                                   chat::Limits{.ping_interval = std::chrono::hours(3),
                                                .idle_timeout = std::chrono::hours(4),
                                                .service = {},
                                                .presence = {}},
                                   true);
    auto alice = open_as("alice");
    ASSERT_TRUE(alice);
    // The token expires an hour after it was checked (FakeVerifier), and a check accepts it for
    // the clock skew past that.
    node_->advance(std::chrono::hours(1) + core::ports::kTokenClockSkew - seconds(1));
    std::vector<std::byte> join;
    ASSERT_TRUE(alice->append(join, codec::ws::Opcode::Text,
                              R"({"type":"join","room":")" + std::string(kRoom) + R"("})"));
    ASSERT_TRUE(alice->send_raw(join));
    const auto answer = alice->next_frame(seconds(10));
    ASSERT_TRUE(answer);
    EXPECT_EQ(answer->first, codec::ws::Opcode::Text) << "closed before its token expired";

    node_->advance(seconds(1));
    std::optional<std::string> close;
    while (const auto frame = alice->next_frame(seconds(10))) {
        if (frame->first == codec::ws::Opcode::Close) {
            close = frame->second;
            break;
        }
    }
    ASSERT_TRUE(close) << "still open after its token expired";
    ASSERT_GE(close->size(), 2U);
    EXPECT_EQ((static_cast<unsigned char>((*close)[0]) << 8U) |
                  static_cast<unsigned char>((*close)[1]),
              4001U);
}

// A token may name any exp the wall clock can hold: its deadline plus the skew must not run past
// the clock's range (signed overflow, caught by UBSan), and such a socket lives on.
TEST_P(ChatSessionTest, ATokenThatExpiresAtTheEndOfTimeKeepsItsSocketOpen) {
    node_.reset();
    node_ = std::make_unique<Node>(GetParam(),
                                   chat::Limits{.ping_interval = std::chrono::hours(3),
                                                .idle_timeout = std::chrono::hours(4),
                                                .service = {},
                                                .presence = {}},
                                   true);
    auto alice = open("Authorization: Bearer forever.alice\r\n");
    ASSERT_TRUE(alice);
    node_->advance(std::chrono::hours(2));
    ASSERT_TRUE(alice->send_text(R"({"type":"join","room":")" + std::string(kRoom) + R"("})"));
    EXPECT_EQ(alice->next_text(seconds(10)),
              R"({"type":"joined","room":")" + std::string(kRoom) + R"(","seq":0})");
}

// The same without UBSan: the deadline of a token at the end of time is the end of the
// monotonic clock, not a sum that wrapped past it.
TEST(TokenDeadline, IsExpPlusTheSkewOnTheMonotonicClockAndNeverWraps) {
    const core::MonoTime now{std::chrono::hours(1000)};
    const core::WallTime wall{std::chrono::seconds(1767225600)};
    EXPECT_EQ(chat::token_deadline(now, wall, wall + std::chrono::hours(1)),
              now + std::chrono::hours(1) + core::ports::kTokenClockSkew);
    EXPECT_EQ(chat::token_deadline(now, wall, wall - seconds(30)), now + seconds(30));
    EXPECT_EQ(chat::token_deadline(now, wall, wall - seconds(90)), now);
    constexpr auto kLast =
        std::chrono::floor<std::chrono::seconds>(core::WallTime::duration::max()) - seconds(1);
    EXPECT_EQ(chat::token_deadline(now, wall, core::WallTime{kLast}),
              now + (core::WallTime{kLast} - wall) + core::ports::kTokenClockSkew);
    // Past what the monotonic clock can count from where it stands: never, not a wrapped sum.
    const core::MonoTime late = core::MonoTime::max() - std::chrono::hours(1);
    EXPECT_EQ(chat::token_deadline(late, wall, wall + std::chrono::hours(2)),
              core::MonoTime::max());
    EXPECT_EQ(chat::token_deadline(late, wall, core::WallTime{kLast}), core::MonoTime::max());
}

TEST_P(ChatSessionTest, ABinaryFrameIsNotSomethingThisProtocolTakes) {
    auto alice = open_as("alice");
    ASSERT_TRUE(alice);
    std::vector<std::byte> binary;
    ASSERT_TRUE(alice->append(binary, codec::ws::Opcode::Binary, "\x01\x02"));
    ASSERT_TRUE(alice->send_raw(binary));
    EXPECT_EQ(alice->close_status(seconds(10)), 1003);
}

TEST_P(ChatSessionTest, AUserJoiningRoomsFasterThanTheLimitIsTurnedAwayOnEveryConnection) {
    node_.reset();
    node_ = std::make_unique<Node>(GetParam(),
                                   chat::Limits{.service = {.join_burst = 2}, .presence = {}});
    const auto join = [](WsClient& ws, std::string_view room) {
        EXPECT_TRUE(ws.send_text(R"({"type":"join","room":")" + std::string(room) + R"("})"));
        const auto answer = ws.next_text(seconds(10));
        return answer.value_or("").find(R"("type":"joined")") != std::string::npos;
    };
    auto first = open_as("alice");
    auto second = open_as("alice");
    auto other = open_as("bob");
    ASSERT_TRUE(first && second && other);
    EXPECT_TRUE(join(*first, "01a0eb86-6cca-7dce-84cc-3bb47615f901"));
    EXPECT_TRUE(join(*first, "01a0eb86-6cca-7dce-84cc-3bb47615f902"));
    EXPECT_FALSE(join(*first, "01a0eb86-6cca-7dce-84cc-3bb47615f903"));
    // A second connection does not reset the user's allowance; another user has their own.
    EXPECT_FALSE(join(*second, "01a0eb86-6cca-7dce-84cc-3bb47615f904"));
    EXPECT_TRUE(join(*other, "01a0eb86-6cca-7dce-84cc-3bb47615f905"));
}

TEST_P(ChatSessionTest, SendsInFlightAreBoundedInBytes) {
    auto alice = open_as("alice");
    ASSERT_TRUE(alice);
    ASSERT_TRUE(alice->send_text(R"({"type":"join","room":")" + std::string(kRoom) + R"("})"));
    ASSERT_TRUE(alice->next_text(seconds(10)));
    // Three sends of 45 KiB while the store answers nothing: two fit the connection's 128 KiB,
    // the third does not, however few sends that is.
    node_->hold_store = true;
    ASSERT_TRUE(ulw::test::eventually([&] { return node_->store_held.load(); }));
    const std::string body =
        infra::auth::encode_base64url(std::string(std::size_t{45} * 1024, 'x'));
    for (int ref = 1; ref <= 3; ++ref) {
        ASSERT_TRUE(alice->send_text(R"({"type":"send","room":")" + std::string(kRoom) +
                                     R"(","id":"m)" + std::to_string(ref) + R"(","body":")" + body +
                                     R"("})"));
    }
    EXPECT_EQ(alice->next_text(seconds(10)), R"({"type":"error","reason":"busy","room":")" +
                                                 std::string(kRoom) + R"(","id":"m3"})");
    node_->hold_store = false;
    int sent = 0;
    while (sent < 2) {
        const auto text = alice->next_text(seconds(10));
        ASSERT_TRUE(text);
        sent += text->starts_with(R"({"type":"sent")") ? 1 : 0;
    }
}

TEST_P(ChatSessionTest, AClientThatAnswersNothingIsClosedAtTheIdleTimeoutNotAPingLater) {
    node_.reset();
    node_ = std::make_unique<Node>(GetParam(),
                                   chat::Limits{.ping_interval = core::Millis{1'000},
                                                .idle_timeout = core::Millis{1'100},
                                                .service = {},
                                                .presence = {}},
                                   true);
    auto quiet = open_as("alice");
    ASSERT_TRUE(quiet);
    // Read, never answer. The node's clock is the test's: it reaches the ping interval, then
    // the idle timeout, at exactly those instants, however slowly the machine runs.
    node_->advance(core::Millis{1'000});
    const auto ping = quiet->next_frame(seconds(10));
    ASSERT_TRUE(ping);
    EXPECT_EQ(ping->first, codec::ws::Opcode::Ping);
    node_->advance(core::Millis{100});
    // Closing at the next ping instead would need another 900 ms of node time, which never
    // comes, so a server that waits for it leaves the connection open until this read times out.
    int later_pings = 0;
    while (const auto frame = quiet->next_frame(seconds(10))) {
        later_pings += frame->first == codec::ws::Opcode::Ping ? 1 : 0;
    }
    EXPECT_EQ(later_pings, 0);
    EXPECT_FALSE(quiet->connected());
}

// The seq of a message frame, or nullopt for anything else.
std::optional<std::uint64_t> message_seq(const std::string& text) {
    const auto json = core::json::parse(text);
    if (!json || json->find("type") == nullptr ||
        json->find("type")->as_string() != std::optional<std::string_view>("message")) {
        return std::nullopt;
    }
    return json->find("seq")->as_u64();
}

TEST_P(ChatSessionTest, AViewerThatStopsReadingSkipsToTheNewestWhileOthersMissNothing) {
    node_.reset();
    // One sender's burst stands in for a busy room's many senders.
    node_ = std::make_unique<Node>(
        GetParam(), chat::Limits{.service = {.send_burst = 1'000,
                                             .max_send_bytes_in_flight = std::size_t{1} << 20U},
                                 .presence = {}});
    auto viewer = open_as("viewer");
    auto reader = open_as("reader");
    auto sender = open_as("sender");
    ASSERT_TRUE(viewer && reader && sender);
    const std::string room = std::string(kRoom);
    ASSERT_TRUE(
        viewer->send_text(R"({"type":"join","room":")" + room + R"(","delivery":"lossy"})"));
    for (auto* ws : {&*reader, &*sender}) {
        ASSERT_TRUE(ws->send_text(R"({"type":"join","room":")" + room + R"("})"));
    }
    for (auto* ws : {&*viewer, &*reader, &*sender}) {
        ASSERT_EQ(ws->next_text(seconds(10)).value_or("").find(R"("type":"joined")"), 1U);
    }

    // The viewer reads nothing from here on. 400 messages of about 2.8 KiB on the wire are
    // 1.1 MiB: past what its socket buffers take (64 KiB of send buffer and the client's
    // receive window), the 64 KiB a lossy client may have queued, and the 64 it is owed.
    constexpr std::uint64_t kMessages = 400;
    const std::string body = infra::auth::encode_base64url(std::string(2'000, 'x'));
    std::uint64_t heard = 0;
    for (std::uint64_t i = 0; i < kMessages; i += 10) {
        for (std::uint64_t k = i; k < i + 10; ++k) {
            ASSERT_TRUE(sender->send_text(std::format(
                R"({{"type":"send","room":"{}","id":"s{}","body":"{}"}})", room, k, body)));
        }
        // The durable reader keeps up, and gets every message in order. So does the sender,
        // which would otherwise be closed for falling behind.
        while (heard < i + 10) {
            const auto text = reader->next_text(seconds(10));
            ASSERT_TRUE(text);
            if (const auto seq = message_seq(*text)) {
                ASSERT_EQ(*seq, ++heard);
            }
        }
        for (std::uint64_t own = 0; own < heard;) {
            const auto text = sender->next_text(seconds(10));
            ASSERT_TRUE(text);
            own = message_seq(*text).value_or(own);
        }
    }

    std::vector<std::uint64_t> seqs;
    while (seqs.empty() || seqs.back() < kMessages) {
        const auto text = viewer->next_text(seconds(10));
        ASSERT_TRUE(text) << "the viewer stopped at " << (seqs.empty() ? 0 : seqs.back());
        if (const auto seq = message_seq(*text)) {
            seqs.push_back(*seq);
        }
    }
    EXPECT_EQ(std::ranges::adjacent_find(seqs, std::ranges::greater_equal{}), seqs.end())
        << "seqs rise, each once";
    EXPECT_LT(seqs.size(), kMessages) << "a viewer that stopped reading was sent everything";
    // What it missed is a gap it can see, and what it got last are the newest, without a hole.
    ASSERT_GE(seqs.size(), 64U);
    EXPECT_EQ(seqs[seqs.size() - 64], kMessages - 63);
    const auto metrics = ulw::test::http_get(node_->port(), "/metrics");
    const std::string drops = "lossy_drops_total ";
    const std::size_t at = metrics.body.find(drops);
    ASSERT_NE(at, std::string::npos);
    EXPECT_EQ(
        metrics.body.substr(at + drops.size(), metrics.body.find('\n', at) - at - drops.size()),
        std::to_string(kMessages - seqs.size()));
    std::cout << "a viewer that stopped reading got " << seqs.size() << " of " << kMessages
              << ", ending with seqs " << seqs[seqs.size() - 64] << ".." << seqs.back()
              << "; the rest counted as dropped\n";
}

// A counter from the node's /metrics.
std::optional<std::uint64_t> metric(std::uint16_t port, std::string_view name) {
    const auto body = ulw::test::http_get(port, "/metrics").body;
    const std::string line = std::string(name) + " ";
    const std::size_t at = body.find(line);
    if (at == std::string::npos) {
        return std::nullopt;
    }
    const std::size_t start = at + line.size();
    return core::parse_integer<std::uint64_t>(
        std::string_view(body).substr(start, body.find('\n', start) - start));
}

// Both viewers fall behind a sender that never stops. One never reads again, and is reset once it
// has acknowledged nothing for the stall timeout, here 2.5 s; the other empties its socket now
// and then, and keeps its connection however long it lags. Real time: the kernel's own timers,
// which decide when a reader's acknowledgements reach the node, run on it, and a test thread
// held up for less than the timeout does not count against the slow one.
TEST_P(ChatSessionTest, AViewerThatAcknowledgesNothingForTheStallTimeoutIsClosedAndASlowOneIsNot) {
    node_.reset();
    node_ = std::make_unique<Node>(
        GetParam(), chat::Limits{.stall_timeout = core::Millis{2'500},
                                 .stall_check = core::Millis{250},
                                 .service = {.send_burst = 1'000'000,
                                             .max_send_bytes_in_flight = std::size_t{1} << 20U},
                                 .presence = {}});
    // A fixed receive buffer: one the kernel tunes keeps growing, to megabytes, for a reader
    // that never takes anything from it, and acknowledges everything it holds.
    auto stopped = WsClient::connect(node_->port(), "/rt", "Authorization: Bearer user.viewer\r\n",
                                     nullptr, 64 * 1024);
    auto slow = open_as("reader");
    auto sender = open_as("sender");
    ASSERT_TRUE(stopped && slow && sender);
    const std::string room = std::string(kRoom);
    for (auto* ws : {&*stopped, &*slow}) {
        ASSERT_TRUE(
            ws->send_text(R"({"type":"join","room":")" + room + R"(","delivery":"lossy"})"));
    }
    ASSERT_TRUE(sender->send_text(R"({"type":"join","room":")" + room + R"("})"));
    for (auto* ws : {&*stopped, &*slow, &*sender}) {
        ASSERT_EQ(ws->next_text(seconds(10)).value_or("").find(R"("type":"joined")"), 1U);
    }

    // One message of about 2.8 KiB on the wire at a time, each heard back by the sender before
    // the next; the slow viewer empties its socket after every tenth. The stopped one's socket
    // buffers and its 64 KiB of queue fill within the first hundred or so.
    const std::string body = infra::auth::encode_base64url(std::string(2'000, 'x'));
    // Everything that has arrived, taken apart as it is read, which also answers the node's
    // pings: the seq of the newest message among it.
    std::uint64_t last = 0;
    const auto empty = [&last](WsClient& ws) {
        while (ws.read_at_most(std::size_t{1} << 20U) > 0) {
            while (const auto text = ws.next_text(std::chrono::milliseconds{0})) {
                last = message_seq(*text).value_or(last);
            }
        }
    };
    const auto deadline = std::chrono::steady_clock::now() + seconds(60);
    std::uint64_t sent = 0;
    std::optional<std::uint64_t> stalled = 0;
    while (stalled == 0U && std::chrono::steady_clock::now() < deadline) {
        ++sent;
        ASSERT_TRUE(sender->send_text(std::format(
            R"({{"type":"send","room":"{}","id":"s{}","body":"{}"}})", room, sent, body)));
        for (std::uint64_t own = 0; own < sent;) {
            const auto text = sender->next_text(seconds(10));
            ASSERT_TRUE(text);
            own = message_seq(*text).value_or(own);
        }
        if (sent % 10 == 0) {
            empty(*slow);
        }
        if (sent % 50 == 0) {
            stalled = metric(node_->port(), "stalled_readers_total");
        }
    }
    ASSERT_EQ(stalled, 1U) << "after " << sent << " messages";

    // The slow one is still there, and is sent the newest message; the stopped one reads what
    // had reached it, and then a reset: the node kept nothing more for it.
    while (last < sent) {
        const auto text = slow->next_text(seconds(10));
        ASSERT_TRUE(text) << "the slow viewer stopped at " << last << " of " << sent;
        last = message_seq(*text).value_or(last);
    }
    while (stopped->next_frame(seconds(10))) {
    }
    EXPECT_FALSE(stopped->connected());
    EXPECT_EQ(stopped->error(), ECONNRESET);
    EXPECT_EQ(metric(node_->port(), "stalled_readers_total"), 1U);
    std::cout << "the stopped viewer was closed after " << sent
              << " messages; the slow one got all it was owed up to seq " << last << "\n";
}

TEST_P(ChatSessionTest, AWatcherHearsAUserArriveAndLeaveOverTheSocket) {
    node_.reset();
    chat::Limits limits;
    limits.presence.grace = core::Millis{200};
    node_ = std::make_unique<Node>(GetParam(), limits);
    auto bob = open_as("bob");
    ASSERT_TRUE(bob);
    ASSERT_TRUE(bob->send_text(R"({"type":"watch","user":"alice"})"));
    EXPECT_EQ(bob->next_text(seconds(10)),
              R"({"type":"watching","user":"alice","status":"offline"})");
    auto alice = open_as("alice");
    ASSERT_TRUE(alice);
    EXPECT_EQ(bob->next_text(seconds(10)),
              R"({"type":"presence","user":"alice","status":"online"})");
    alice.reset();
    EXPECT_EQ(bob->next_text(seconds(10)),
              R"({"type":"presence","user":"alice","status":"offline"})");
    ASSERT_TRUE(bob->send_text(R"({"type":"watch","user":"not a user"})"));
    EXPECT_EQ(bob->next_text(seconds(10)), R"({"type":"error","reason":"bad_user"})");
}

// A Ping and a join, masked, as a client would send them right behind its upgrade request.
std::vector<std::byte> first_frames() {
    ulw::test::FakeRandom random;
    codec::ws::ClientEncoder encoder(random);
    std::vector<std::byte> out;
    for (const auto& [opcode, payload] :
         {std::pair{codec::ws::Opcode::Ping, std::string{"early"}},
          std::pair{codec::ws::Opcode::Text,
                    R"({"type":"join","room":")" + std::string(kRoom) + R"("})"}}) {
        const auto bytes = std::as_bytes(std::span{payload});
        EXPECT_TRUE(encoder.encode({.opcode = opcode,
                                    .fin = true,
                                    .payload = {bytes.begin(), bytes.end()},
                                    .close_code = codec::ws::CloseCode::NoStatus},
                                   out));
    }
    return out;
}

void expect_first_frames_answered(WsClient& ws) {
    const auto pong = ws.next_frame(seconds(10));
    ASSERT_TRUE(pong);
    EXPECT_EQ(pong->first, codec::ws::Opcode::Pong);
    EXPECT_EQ(pong->second, "early");
    EXPECT_EQ(ws.next_text(seconds(10)),
              R"({"type":"joined","room":")" + std::string(kRoom) + R"(","seq":0})");
}

TEST_P(ChatSessionTest, TheHttpParserIsFreedOnceTheRequestIsAnswered) {
    // A request still arriving keeps its parser...
    const os::UniqueFd partial{::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0)};
    ASSERT_TRUE(partial);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(node_->port());
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    // connect() takes every address family through the generic sockaddr header.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    ASSERT_EQ(::connect(partial.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof addr), 0);
    const std::string_view line = "GET /rt HTTP/1.1\r\n";
    ASSERT_EQ(::send(partial.get(), line.data(), line.size(), MSG_NOSIGNAL),
              static_cast<ssize_t>(line.size()));
    ASSERT_TRUE(ulw::test::eventually([&] { return node_->http_parsers == 1; }));
    // ...and one answered, upgraded or refused, holds none, whatever it goes on to do.
    auto alice = open_as("alice");
    auto bob = open_as("bob");
    ASSERT_TRUE(alice && bob);
    EXPECT_EQ(refusal("Authorization: Bearer forged\r\n"), "HTTP/1.1 401 Unauthorized");
    EXPECT_EQ(ulw::test::http_get(node_->port(), "/healthz").status, 200);
    EXPECT_TRUE(ulw::test::eventually([&] { return node_->http_parsers == 1; }));
    ASSERT_TRUE(alice->send_text(R"({"type":"join","room":")" + std::string(kRoom) + R"("})"));
    EXPECT_EQ(alice->next_text(seconds(10)),
              R"({"type":"joined","room":")" + std::string(kRoom) + R"(","seq":0})");
    EXPECT_EQ(node_->http_parsers, 1U);
}

TEST_P(ChatSessionTest, FramesSentRightBehindTheUpgradeRequestAreRead) {
    const auto frames = first_frames();
    auto alice = WsClient::connect(node_->port(), "/rt", "Authorization: Bearer user.alice\r\n",
                                   nullptr, frames);
    ASSERT_TRUE(alice);
    expect_first_frames_answered(*alice);
    EXPECT_TRUE(ulw::test::eventually([&] { return node_->http_parsers == 0; }));
}

TEST_P(ChatSessionTest, FramesSentBehindAnUpgradeWaitingOnKeysAreReadOnceItIsAccepted) {
    const auto frames = first_frames();
    auto upgrade = std::async(std::launch::async, [&] {
        return WsClient::connect(node_->port(), "/rt", "Authorization: Bearer slow.alice\r\n",
                                 nullptr, frames);
    });
    ASSERT_TRUE(ulw::test::eventually([&] { return node_->key_waiters == 1; }));
    node_->refresh_keys = true;
    auto alice = upgrade.get();
    ASSERT_TRUE(alice);
    expect_first_frames_answered(*alice);
    EXPECT_TRUE(ulw::test::eventually([&] { return node_->http_parsers == 0; }));
}

TEST_P(ChatSessionTest, AnUpgradeThatFailedWhileWaitingOnKeysIsNotAcceptedWhenTheyArrive) {
    auto upgrade = std::async(std::launch::async, [&] {
        std::string status;
        const bool opened =
            WsClient::connect(node_->port(), "/rt", "Authorization: Bearer slow.alice\r\n", &status)
                .has_value();
        return std::pair{opened, status};
    });
    ASSERT_TRUE(ulw::test::eventually([&] { return node_->key_waiters == 1; }));
    node_->fail_then_refresh_keys = true;
    const auto [opened, status] = upgrade.get();
    // Closed without a response: no 101 after the failure.
    EXPECT_FALSE(opened);
    EXPECT_EQ(status, "");
}

INSTANTIATE_TEST_SUITE_P(Reactors, ChatSessionTest,
                         ::testing::Values(net::ReactorKind::IoUring, net::ReactorKind::Epoll),
                         ulw::test::reactor_name);

} // namespace
