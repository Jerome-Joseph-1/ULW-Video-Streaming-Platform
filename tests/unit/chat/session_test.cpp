#include "infra/auth/base64url.hpp"
#include "infra/messages/memory_message_store.hpp"
#include "net/reactor_factory.hpp"
#include "net/socket.hpp"
#include "os/system_clock.hpp"
#include "os/system_random.hpp"
#include "rt/room_router.hpp"

#include "chat.hpp"
#include "support/eventually.hpp"
#include "support/fake_verifier.hpp"
#include "support/reactor_harness.hpp"
#include "support/ws_client.hpp"
#include "unit/rt/memory_room_store.hpp"

#include <atomic>
#include <future>
#include <gtest/gtest.h>
#include <memory>
#include <optional>
#include <string>
#include <thread>

namespace {

using std::chrono::seconds;
using ulw::test::WsClient;

constexpr std::string_view kRoom = "01a0eb86-6cca-7dce-84cc-3bb47615f9fd";
constexpr std::string_view kAllowed = "https://app.askedin.test";

// One chat node on a thread of its own, as chat_server runs it, over an in-memory room store.
// The test talks to it through sockets only.
class Node {
public:
    explicit Node(net::ReactorKind kind, chat::Limits limits = {}) : limits_(limits) {
        std::promise<std::uint16_t> port;
        auto ready = port.get_future();
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
    // While set, the room store answers nothing: every send stays in flight. `store_held`
    // follows once the node's thread has seen it.
    std::atomic<bool> hold_store = false;
    std::atomic<bool> store_held = false;

private:
    // An io_uring reactor belongs to the thread that made it, so everything is made here.
    void run(net::ReactorKind kind, std::promise<std::uint16_t>& port) {
        os::SystemClock clock;
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
        while (!stop_) {
            if (hold_store != store->hold) {
                if (hold_store) {
                    store->hold = true;
                } else {
                    store->release_held();
                }
                store_held = store->hold;
            }
            (*reactor)->run_once(core::Millis{5});
            server->reap();
        }
        messages.reset();
        server.reset();
        store.reset();
    }

    chat::Limits limits_;
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
    EXPECT_TRUE(ulw::test::eventually(
        [&] { return ulw::test::http_get(node_->port(), "/readyz").status == 200; }));
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
    node_ = std::make_unique<Node>(GetParam(), chat::Limits{.ping_interval = core::Millis{1'000},
                                                            .idle_timeout = core::Millis{1'100},
                                                            .service = {},
                                                            .presence = {}});
    // Taken before the upgrade: the server counts quiet from the upgrade request, so a clock
    // started after the handshake returns would miss however long that took on a loaded box.
    const auto opened = std::chrono::steady_clock::now();
    auto quiet = open_as("alice");
    ASSERT_TRUE(quiet);
    // Read, never answer: the pings go unanswered.
    int pings = 0;
    while (const auto frame = quiet->next_frame(seconds(10))) {
        pings += frame->first == codec::ws::Opcode::Ping ? 1 : 0;
    }
    const auto lasted = std::chrono::steady_clock::now() - opened;
    EXPECT_EQ(pings, 1);
    // At the idle timeout, 1.1 s, rounded up by the timer wheel's 100 ms ticks. Closing at the
    // next ping instead would take 2 s, so the bound sits just under that: 800 ms of slack for a
    // loaded machine, on the real clock the reactor's timers run on.
    EXPECT_GE(lasted, std::chrono::milliseconds(1'100));
    EXPECT_LT(lasted, std::chrono::milliseconds(1'900));
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

INSTANTIATE_TEST_SUITE_P(Reactors, ChatSessionTest,
                         ::testing::Values(net::ReactorKind::IoUring, net::ReactorKind::Epoll),
                         ulw::test::reactor_name);

} // namespace
