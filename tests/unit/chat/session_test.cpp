#include "core/util/json.hpp"
#include "core/util/parse.hpp"
#include "infra/auth/base64url.hpp"
#include "infra/messages/memory_message_store.hpp"
#include "net/ip_address.hpp"
#include "net/reactor_factory.hpp"
#include "net/signals.hpp"
#include "net/socket.hpp"
#include "os/system_clock.hpp"
#include "os/system_random.hpp"
#include "os/unique_fd.hpp"
#include "rt/room_router.hpp"

#include "chat.hpp"
#include "session.hpp"
#include "support/eventually.hpp"
#include "support/fake_clock.hpp"
#include "support/fake_push_transport.hpp"
#include "support/fake_random.hpp"
#include "support/fake_verifier.hpp"
#include "support/memory_push_store.hpp"
#include "support/reactor_harness.hpp"
#include "support/ws_client.hpp"
#include "unit/rt/memory_room_store.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <filesystem>
#include <format>
#include <future>
#include <gtest/gtest.h>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <poll.h>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using std::chrono::seconds;
using ulw::test::WsClient;

constexpr std::string_view kRoom = "01a0eb86-6cca-7dce-84cc-3bb47615f9fd";
constexpr std::string_view kAllowed = "https://app.example.com";

// Hands the server each connection as if it came from the peer `named` holds, while it holds
// one: the loopback interface offers only 127.0.0.1, and on some hosts not even ::1.
class NamedPeers final : public net::IAcceptHandler {
public:
    NamedPeers(chat::ChatServer& server, std::mutex& lock,
               const std::optional<net::IpAddress>& named) noexcept
        : server_(server), lock_(lock), named_(named) {}

    void on_accept(os::UniqueFd conn) noexcept override {
        std::optional<net::IpAddress> peer;
        {
            const std::scoped_lock held(lock_);
            peer = named_;
        }
        if (peer) {
            server_.accept_from(std::move(conn), *peer);
        } else {
            server_.on_accept(std::move(conn));
        }
    }

private:
    chat::ChatServer& server_;
    std::mutex& lock_;
    const std::optional<net::IpAddress>& named_;
};

// One chat node on a thread of its own, as chat_server runs it, over an in-memory room store.
// The test talks to it through sockets only.
class Node {
public:
    // With `manual_clock`, time on the node stands still until advance() moves it.
    // With `push`, Web Push is configured, on a store in memory and a sender whose push service
    // answers nothing.
    explicit Node(net::ReactorKind kind, chat::Limits limits = {}, bool manual_clock = false,
                  bool push = false)
        : limits_(std::move(limits)), manual_clock_(manual_clock), push_(push) {
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

    // The peer the node takes the next connections to come from; nullopt for their own.
    void name_peer(std::optional<net::IpAddress> peer) {
        const std::scoped_lock held(peer_lock_);
        named_peer_ = peer;
    }

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
    // What the verifier says of keys_expired(); `keys_expired` follows once the node's thread has
    // seen it.
    std::atomic<bool> expire_keys = false;
    std::atomic<bool> keys_expired = false;
    // Delivers SIGHUP to the server on the node's next turn; `verifier_drops` then counts the
    // verifier's drop_caches() calls.
    std::atomic<bool> sighup = false;
    // What the verifier reports as drop_pending(), applied on the node's next turn.
    std::atomic<bool> drop_pending = false;
    std::atomic<bool> drop_pending_seen = false;
    std::atomic<std::size_t> verifier_drops = 0;

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
        ulw::test::MemoryPushStore push_store;
        const auto vapid = infra::webpush::VapidKey::from_private(infra::webpush::PrivateKey{3});
        infra::webpush::PushSender sender(std::make_unique<ulw::test::FakePushTransport>(), *vapid,
                                          clock, {.subject = "mailto:ops@example.com"});
        chat::PushDeps push_deps{.store = push_store,
                                 .sender = sender,
                                 .key = *vapid,
                                 .hosts = infra::webpush::PushHosts::defaults(),
                                 .limits = {}};
        auto server = std::make_unique<chat::ChatServer>(
            chat::Deps{.node = *core::NodeId::parse("chat-1"),
                       .reactor = **reactor,
                       .router = router,
                       .messages = *messages,
                       .verifier = verifier,
                       .clock = clock,
                       .random = random,
                       .sfu = nullptr,
                       .push = push_ ? &push_deps : nullptr},
            chat::Access{.cookie = "auth_token", .allowed_origins = {std::string(kAllowed)}},
            limits_);
        NamedPeers accept(*server, peer_lock_, named_peer_);
        if (!(*reactor)->listen(std::move(*clients), accept)) {
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
            push_store.flush();
            if (refresh_keys.exchange(false)) {
                verifier.refresh_keys();
            }
            if (fail_then_refresh_keys.exchange(false)) {
                server->for_each_session([](chat::Session& s) noexcept { s.allocation_failed(); });
                verifier.refresh_keys();
            }
            if (expire_keys != verifier.expired) {
                verifier.expired = expire_keys;
                keys_expired = verifier.expired;
            }
            if (sighup.exchange(false)) {
                server->on_signal(net::Signal::Reload);
            }
            verifier_drops = verifier.drops;
            verifier.drop_requested = drop_pending;
            drop_pending_seen = verifier.drop_requested;
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
    bool push_;
    std::mutex peer_lock_;
    std::optional<net::IpAddress> named_peer_;
    std::promise<void> healthy_promise_;
    std::future<void> healthy_;
    std::atomic<std::int64_t> requested_ = 0;
    std::atomic<std::int64_t> applied_ = 0;
    std::atomic<std::uint64_t> turns_ = 0;
    std::atomic<bool> stop_ = false;
    std::uint16_t port_ = 0;
    std::jthread thread_;
};

// An upgrade request on a connection the test keeps open, and the head of the answer.
struct Asked {
    os::UniqueFd conn;
    std::string head;
};

// The value of a header in an answer's head, whole: "5" for "Retry-After: 5", never the "5" of
// "Retry-After: 50". Empty when the head has no such header.
std::string header_value(const std::string& head, std::string_view name) {
    const std::string key = "\r\n" + std::string(name) + ": ";
    const auto at = head.find(key);
    if (at == std::string::npos) {
        return {};
    }
    const auto from = at + key.size();
    const auto end = head.find("\r\n", from);
    return head.substr(from, end == std::string::npos ? std::string::npos : end - from);
}

Asked ask_upgrade(std::uint16_t port, const std::string& headers) {
    Asked out{.conn = os::UniqueFd{::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0)}, .head = {}};
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (!out.conn) {
        return out;
    }
    // connect() takes every address family through the generic sockaddr header.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    if (::connect(out.conn.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof addr) != 0) {
        return out;
    }
    const std::string request = "GET /rt HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\n"
                                "Connection: Upgrade\r\n"
                                "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                                "Sec-WebSocket-Version: 13\r\n" +
                                headers + "\r\n";
    if (::send(out.conn.get(), request.data(), request.size(), MSG_NOSIGNAL) !=
        static_cast<ssize_t>(request.size())) {
        return out;
    }
    std::string in;
    std::array<char, 4096> buf{};
    pollfd pfd{.fd = out.conn.get(), .events = POLLIN, .revents = 0};
    while (in.find("\r\n\r\n") == std::string::npos && ::poll(&pfd, 1, 10'000) > 0) {
        const ssize_t n = ::recv(out.conn.get(), buf.data(), buf.size(), 0);
        if (n <= 0) {
            break;
        }
        in.append(buf.data(), static_cast<std::size_t>(n));
    }
    out.head = in.substr(0, in.find("\r\n\r\n"));
    return out;
}

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

    // A counter from the node's /metrics, asked from an address no test connects from, so that
    // the limits a test has filled neither refuse the request nor count it.
    std::optional<std::uint64_t> counter(std::string_view name) {
        node_->name_peer(net::IpAddress::parse("192.0.2.250"));
        const auto body = ulw::test::http_get(node_->port(), "/metrics").body;
        node_->name_peer(std::nullopt);
        const std::string line = std::string(name) + " ";
        const std::size_t at = body.find(line);
        if (at == std::string::npos) {
            return std::nullopt;
        }
        const std::size_t start = at + line.size();
        return core::parse_integer<std::uint64_t>(
            std::string_view(body).substr(start, body.find('\n', start) - start));
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
    // A socket the kernel or the reactor would not take is not a full node, and is counted
    // apart, as the gateway counts it.
    EXPECT_NE(metrics.body.find("connections_rejected_total{reason=\"socket\"} 0\n"),
              std::string::npos);
    EXPECT_NE(metrics.body.find("connections_rejected_total{reason=\"capacity\"} 0\n"),
              std::string::npos);
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

// Every frame is encoded into one buffer the server keeps, and every message's text into one
// the service keeps: a large message leaves nothing of itself in a smaller one sent after it.
TEST_P(ChatSessionTest, AMessageAfterALargerOneIsSentExactlyToEveryMember) {
    auto alice = open_as("alice");
    auto bob = open_as("bob");
    ASSERT_TRUE(alice);
    ASSERT_TRUE(bob);
    const std::string join = R"({"type":"join","room":")" + std::string(kRoom) + R"("})";
    ASSERT_TRUE(alice->send_text(join));
    EXPECT_EQ(alice->next_text(seconds(10)),
              R"({"type":"joined","room":")" + std::string(kRoom) + R"(","seq":0})");
    ASSERT_TRUE(bob->send_text(join));
    EXPECT_EQ(bob->next_text(seconds(10)),
              R"({"type":"joined","room":")" + std::string(kRoom) + R"(","seq":0})");
    const std::string large =
        infra::auth::encode_base64url(std::string(std::size_t{24} * 1024, 'L'));
    const std::string small = "cw";
    std::uint64_t seq = 0;
    for (const std::string* body : {&large, &small, &large, &small}) {
        ++seq;
        const std::string id = "m" + std::to_string(seq);
        ASSERT_TRUE(alice->send_text(R"({"type":"send","room":")" + std::string(kRoom) +
                                     R"(","id":")" + id + R"(","body":")" + *body + R"("})"));
        const std::string message = R"({"type":"message","room":")" + std::string(kRoom) +
                                    R"(","seq":)" + std::to_string(seq) +
                                    R"(,"sender":"alice","id":")" + id + R"(","body":")" + *body +
                                    R"("})";
        EXPECT_EQ(alice->next_text(seconds(10)), message) << id;
        EXPECT_EQ(alice->next_text(seconds(10)), R"({"type":"sent","room":")" + std::string(kRoom) +
                                                     R"(","id":")" + id + R"(","seq":)" +
                                                     std::to_string(seq) + "}")
            << id;
        EXPECT_EQ(bob->next_text(seconds(10)), message) << id;
    }
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

// A valid token does not carry a request that is not a WebSocket handshake.
TEST_P(ChatSessionTest, AnUpgradeThatBreaksTheHandshakeIsRefusedWhateverItsToken) {
    const std::string token = "Authorization: Bearer user.alice\r\n";
    // WsClient sends one of each already; a second is a handshake error, even one that is
    // well formed on its own (16 zero bytes in base64).
    const std::string second_key = "Sec-WebSocket-Key: " + std::string(22, 'A') + "==\r\n";
    EXPECT_EQ(refusal(token + second_key), "HTTP/1.1 400 Bad Request");
    EXPECT_EQ(refusal(token + "Sec-WebSocket-Version: 13\r\n"), "HTTP/1.1 426 Upgrade Required");
    // Nothing here takes a body.
    EXPECT_EQ(refusal(token + "Content-Length: 5\r\n"), "HTTP/1.1 400 Bad Request");
    EXPECT_TRUE(open_as("alice"));
}

// RFC 6455 section 5: a client's frame unmasked, with an opcode no one defined, or with a
// reserved bit no extension negotiated, fails the connection with 1002, and the node counts it.
TEST_P(ChatSessionTest, AFrameThatBreaksTheProtocolClosesWith1002AndIsCounted) {
    const std::vector<std::vector<unsigned char>> frames{
        {0x81, 0x02, 'h', 'i'},
        {0x83, 0x80, 0x01, 0x02, 0x03, 0x04},
        {0xC1, 0x80, 0x01, 0x02, 0x03, 0x04},
    };
    for (const auto& frame : frames) {
        auto alice = open_as("alice");
        ASSERT_TRUE(alice);
        ASSERT_TRUE(alice->send_raw(std::as_bytes(std::span(frame))));
        EXPECT_EQ(alice->close_status(seconds(10)), 1002) << static_cast<int>(frame[0]);
    }
    const auto metrics = ulw::test::http_get(node_->port(), "/metrics");
    EXPECT_NE(metrics.body.find("protocol_errors_total 3\n"), std::string::npos) << metrics.body;
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

// The timer that should send the ping runs late: the node's clock has already passed the ping
// interval, not reached it exactly.
TEST_P(ChatSessionTest, AQuietClientIsPingedEvenWhenTheTimerRunsLate) {
    node_.reset();
    node_ = std::make_unique<Node>(GetParam(),
                                   chat::Limits{.ping_interval = core::Millis{1'000},
                                                .idle_timeout = core::Millis{5'000},
                                                .service = {},
                                                .presence = {}},
                                   true);
    auto quiet = open_as("alice");
    ASSERT_TRUE(quiet);
    node_->advance(core::Millis{1'500});
    const auto ping = quiet->next_frame(seconds(10));
    ASSERT_TRUE(ping);
    EXPECT_EQ(ping->first, codec::ws::Opcode::Ping);
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

// A member that asked never to miss a message and stops reading is closed once its unread output
// passes max_backlog, and resumes from its last seq when it comes back.
TEST_P(ChatSessionTest, AMemberThatStopsReadingIsClosedAsASlowConsumer) {
    node_.reset();
    node_ = std::make_unique<Node>(
        GetParam(), chat::Limits{.service = {.send_burst = 1'000,
                                             .max_send_bytes_in_flight = std::size_t{1} << 20U},
                                 .presence = {}});
    auto stopped = WsClient::connect(node_->port(), "/rt", "Authorization: Bearer user.reader\r\n",
                                     nullptr, 16 * 1024);
    auto sender = open_as("sender");
    ASSERT_TRUE(stopped && sender);
    const std::string room = std::string(kRoom);
    for (auto* ws : {&*stopped, &*sender}) {
        ASSERT_TRUE(ws->send_text(R"({"type":"join","room":")" + room + R"("})"));
        ASSERT_EQ(ws->next_text(seconds(10)).value_or("").find(R"("type":"joined")"), 1U);
    }
    // About 2.8 KiB a message on the wire: 200 of them are twice max_backlog, past the socket
    // buffers too. The sender reads its own, so only the stopped member falls behind.
    const std::string body = infra::auth::encode_base64url(std::string(2'000, 'x'));
    std::uint64_t heard = 0;
    for (std::uint64_t k = 1; k <= 200; ++k) {
        ASSERT_TRUE(sender->send_text(
            std::format(R"({{"type":"send","room":"{}","id":"s{}","body":"{}"}})", room, k, body)));
        while (heard < k) {
            const auto text = sender->next_text(seconds(10));
            ASSERT_TRUE(text);
            heard = message_seq(*text).value_or(heard);
        }
    }
    EXPECT_TRUE(ulw::test::eventually(
        [&] { return metric(node_->port(), "slow_consumers_total") == std::uint64_t{1}; }));
}

// Once the keys go unrefreshed too long every token is refused, so the gauge an alert watches
// says so while it lasts.
TEST_P(ChatSessionTest, TheKeysExpiredGaugeFollowsTheVerifier) {
    EXPECT_EQ(metric(node_->port(), "jwks_keys_expired"), 0U);
    node_->expire_keys = true;
    ASSERT_TRUE(ulw::test::eventually([&] { return node_->keys_expired.load(); }));
    EXPECT_EQ(metric(node_->port(), "jwks_keys_expired"), 1U);
    node_->expire_keys = false;
    ASSERT_TRUE(ulw::test::eventually([&] { return !node_->keys_expired.load(); }));
    EXPECT_EQ(metric(node_->port(), "jwks_keys_expired"), 0U);
}

// SIGHUP is how the identity provider's key rotation reaches chat_server (ADR-0082): the verifier
// is asked to drop its keys and verdicts, /metrics counts it, and the gauge follows the verifier's
// pending drop.
TEST_P(ChatSessionTest, SighupRequestsAnAuthCacheDrop) {
    EXPECT_EQ(metric(node_->port(), "auth_cache_drops_total"), 0U);
    EXPECT_EQ(metric(node_->port(), "auth_cache_drop_pending"), 0U);
    node_->sighup = true;
    ASSERT_TRUE(ulw::test::eventually([&] { return node_->verifier_drops.load() == 1; }));
    EXPECT_EQ(metric(node_->port(), "auth_cache_drops_total"), 1U);
    node_->drop_pending = true;
    ASSERT_TRUE(ulw::test::eventually([&] { return node_->drop_pending_seen.load(); }));
    EXPECT_EQ(metric(node_->port(), "auth_cache_drop_pending"), 1U);
    node_->drop_pending = false;
    ASSERT_TRUE(ulw::test::eventually([&] { return !node_->drop_pending_seen.load(); }));
    EXPECT_EQ(metric(node_->port(), "auth_cache_drop_pending"), 0U);
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
                                 // It reads the metrics on a new connection every 50 messages,
                                 // from the one address every client here shares.
                                 .new_connections_per_ip_per_second = 1'000,
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

// The node's own ends of its accepted client connections: sockets in this process whose local
// port is the node's client port and which have a peer. The listener has none.
std::vector<int> accepted_ends(std::uint16_t port) {
    std::vector<int> out;
    for (const auto& entry : std::filesystem::directory_iterator("/proc/self/fd")) {
        const auto fd = core::parse_integer<int>(entry.path().filename().string());
        if (!fd) {
            continue;
        }
        sockaddr_in local{};
        sockaddr_in peer{};
        socklen_t local_len = sizeof local;
        socklen_t peer_len = sizeof peer;
        // Both take every address family through the generic sockaddr header.
        // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)
        if (::getsockname(*fd, reinterpret_cast<sockaddr*>(&local), &local_len) == 0 &&
            local.sin_family == AF_INET && ntohs(local.sin_port) == port &&
            ::getpeername(*fd, reinterpret_cast<sockaddr*>(&peer), &peer_len) == 0) {
            out.push_back(*fd);
        }
        // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
    }
    return out;
}

// Linux counts a shut receive window against TCP_USER_TIMEOUT from the first window probe, so a
// viewer that reads, but frees its window a little at a time, would be ended by the kernel as if
// it had vanished. The session bounds a stall itself (the test above); the kernel's timer stays
// off on a client's connection.
TEST_P(ChatSessionTest, AClientConnectionIsNotEndedByTheKernelsUserTimeout) {
    auto alice = open_as("alice");
    ASSERT_TRUE(alice);
    const auto ends = accepted_ends(node_->port());
    ASSERT_EQ(ends.size(), 1U);
    int timeout = -1;
    socklen_t len = sizeof timeout;
    ASSERT_EQ(::getsockopt(ends.front(), IPPROTO_TCP, TCP_USER_TIMEOUT, &timeout, &len), 0);
    EXPECT_EQ(timeout, 0);
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

// Reading stops while the upgrade waits on keys; it starts again once the upgrade is accepted.
TEST_P(ChatSessionTest, FramesSentAfterAnUpgradeThatWaitedOnKeysAreRead) {
    auto upgrade = std::async(std::launch::async, [&] {
        return WsClient::connect(node_->port(), "/rt", "Authorization: Bearer slow.alice\r\n",
                                 nullptr);
    });
    ASSERT_TRUE(ulw::test::eventually([&] { return node_->key_waiters == 1; }));
    node_->refresh_keys = true;
    auto alice = upgrade.get();
    ASSERT_TRUE(alice);
    ASSERT_TRUE(alice->send_text(R"({"type":"join","room":")" + std::string(kRoom) + R"("})"));
    EXPECT_EQ(alice->next_text(seconds(10)),
              R"({"type":"joined","room":")" + std::string(kRoom) + R"(","seq":0})");
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

// ADR-0076: one address that connects directly holds at most max_connections_per_ip sockets,
// counted from accept, and is reset before a byte is read past that.
TEST_P(ChatSessionTest, ADirectPeerHoldsAtMostItsAddressesConnections) {
    node_.reset();
    chat::Limits limits;
    limits.max_connections_per_ip = 3;
    node_ = std::make_unique<Node>(GetParam(), limits);
    std::vector<WsClient> open_ones;
    for (const char* user : {"alice", "bob", "carol"}) {
        auto ws = open_as(user);
        ASSERT_TRUE(ws) << user;
        open_ones.push_back(std::move(*ws));
    }
    // Reset at accept: no status line, and no token looked at.
    EXPECT_EQ(refusal("Authorization: Bearer user.dave\r\n"), "");
    EXPECT_EQ(counter(R"(connections_rejected_total{reason="ip_connections"})"), 1U);
    EXPECT_EQ(counter(R"(connections_rejected_total{reason="ip_rate"})"), 0U);
    // Closing one gives its place back.
    open_ones.pop_back();
    EXPECT_TRUE(ulw::test::eventually([&] { return open_as("dave").has_value(); }));
}

// X-Forwarded-For is believed from a trusted proxy only: a direct peer that sends one is still
// counted as its own address, or it could name a fresh one for every connection.
TEST_P(ChatSessionTest, ADirectPeerIsCountedAsItsOwnAddressWhateverItsXForwardedForSays) {
    node_.reset();
    chat::Limits limits;
    limits.max_connections_per_ip = 2;
    node_ = std::make_unique<Node>(GetParam(), limits);
    auto first = open("X-Forwarded-For: 203.0.113.1\r\nAuthorization: Bearer user.alice\r\n");
    auto second = open("X-Forwarded-For: 203.0.113.2\r\nAuthorization: Bearer user.bob\r\n");
    ASSERT_TRUE(first && second);
    EXPECT_EQ(refusal("X-Forwarded-For: 203.0.113.3\r\nAuthorization: Bearer user.carol\r\n"), "");
    EXPECT_EQ(counter(R"(connections_rejected_total{reason="ip_connections"})"), 1U);
    EXPECT_EQ(counter(R"(upgrades_limited_total{limit="ip"})"), 0U);
}

// A customer handed a /56 or a /48 has hundreds of /64s, each of which max_connections_per_ip
// counts afresh: the /64s of one /48 are held to max_connections_per_ip_block together.
TEST_P(ChatSessionTest, TheSlash64sOfOneIpv6Slash48TogetherHoldAtMostItsBlocksConnections) {
    node_.reset();
    chat::Limits limits;
    limits.max_connections_per_ip = 2;
    limits.max_connections_per_ip_block = 3;
    node_ = std::make_unique<Node>(GetParam(), limits);
    std::vector<WsClient> open_ones;
    for (const char* peer : {"2001:db8:1:1::1", "2001:db8:1:2::1", "2001:db8:1:3::1"}) {
        node_->name_peer(net::IpAddress::parse(peer));
        auto ws = open_as("alice");
        ASSERT_TRUE(ws) << peer;
        open_ones.push_back(std::move(*ws));
    }
    // A fourth /64 of the block, with none of its own open, is reset at accept.
    const auto fourth = net::IpAddress::parse("2001:db8:1:ff::1");
    node_->name_peer(fourth);
    EXPECT_EQ(refusal("Authorization: Bearer user.bob\r\n"), "");
    // Another /48 is not held to it.
    node_->name_peer(net::IpAddress::parse("2001:db8:2:1::1"));
    auto elsewhere = open_as("bob");
    EXPECT_TRUE(elsewhere);
    EXPECT_EQ(counter(R"(connections_rejected_total{reason="ip_block"})"), 1U);
    EXPECT_EQ(counter(R"(connections_rejected_total{reason="ip_connections"})"), 0U);
    EXPECT_EQ(counter(R"(rate_limit_entries{table="ip_block"})"), 2U);
    // Closing one gives the block its place back.
    open_ones.pop_back();
    node_->name_peer(fourth);
    EXPECT_TRUE(ulw::test::eventually([&] { return open_as("bob").has_value(); }));
}

TEST_P(ChatSessionTest, ADirectPeerOpensAtMostItsNewConnectionsASecond) {
    node_.reset();
    chat::Limits limits;
    limits.new_connections_per_ip_per_second = 2;
    node_ = std::make_unique<Node>(GetParam(), limits, true);
    auto one = open_as("alice");
    auto two = open_as("bob");
    ASSERT_TRUE(one && two);
    EXPECT_EQ(refusal("Authorization: Bearer user.carol\r\n"), "");
    // One more every half second.
    node_->advance(core::Millis{500});
    EXPECT_TRUE(open_as("carol"));
    EXPECT_EQ(refusal("Authorization: Bearer user.dave\r\n"), "");
    EXPECT_EQ(counter(R"(connections_rejected_total{reason="ip_rate"})"), 2U);
    EXPECT_EQ(counter(R"(connections_rejected_total{reason="ip_connections"})"), 0U);
}

// One valid token must not take the node's every socket.
TEST_P(ChatSessionTest, AUserHoldsAtMostItsSessionsAndIsToldWhenToComeBack) {
    node_.reset();
    chat::Limits limits;
    limits.max_sessions_per_user = 2;
    node_ = std::make_unique<Node>(GetParam(), limits);
    auto first = open_as("alice");
    auto second = open_as("alice");
    ASSERT_TRUE(first && second);
    // A socket of the user's closing frees a place; a few seconds is enough.
    const auto third = ask_upgrade(node_->port(), "Authorization: Bearer user.alice\r\n");
    EXPECT_TRUE(third.head.starts_with("HTTP/1.1 429 Too Many Requests\r\n")) << third.head;
    EXPECT_EQ(header_value(third.head, "Retry-After"), "5") << third.head;
    // Another user is not held to alice's count.
    EXPECT_TRUE(open_as("bob"));
    EXPECT_EQ(counter(R"(upgrades_limited_total{limit="user_sessions"})"), 1U);
    EXPECT_EQ(counter(R"(rate_limit_entries{table="user"})"), 2U);
    first.reset();
    EXPECT_TRUE(ulw::test::eventually([&] { return open_as("alice").has_value(); }));
}

// A socket closed because its token ran out (4001) gives its user's place back, so the client
// that reconnects with a fresh token, as the code tells it to, is not refused for its own
// expired socket.
TEST_P(ChatSessionTest, ASocketClosedForAnExpiredTokenGivesItsUsersPlaceBack) {
    node_.reset();
    node_ = std::make_unique<Node>(GetParam(),
                                   chat::Limits{.ping_interval = std::chrono::hours(3),
                                                .idle_timeout = std::chrono::hours(4),
                                                // Time stands still: the reconnects below
                                                // would otherwise run the bucket dry.
                                                .new_connections_per_ip_per_second = 1'000,
                                                .max_sessions_per_user = 2,
                                                .service = {},
                                                .presence = {}},
                                   true);
    auto first = open_as("alice");
    ASSERT_TRUE(first);
    node_->advance(std::chrono::minutes(30));
    auto second = open_as("alice");
    ASSERT_TRUE(second);
    EXPECT_EQ(refusal("Authorization: Bearer user.alice\r\n"), "HTTP/1.1 429 Too Many Requests");
    // The first token expires an hour after it was checked (FakeVerifier), plus the clock skew;
    // the second half an hour later.
    node_->advance(std::chrono::minutes(30) + core::ports::kTokenClockSkew);
    std::optional<std::string> close;
    while (const auto frame = first->next_frame(seconds(10))) {
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
    first.reset();
    EXPECT_TRUE(ulw::test::eventually([&] { return open_as("alice").has_value(); }));
}

// Behind a trusted proxy every connection comes from the proxy; the address it names is held to
// max_connections_per_ip for its upgrades not yet answered, and no longer: a socket that is open
// is its user's to count.
TEST_P(ChatSessionTest, BehindATrustedProxyTheForwardedAddressIsHeldOnlyUntilItsUpgradeIsAnswered) {
    node_.reset();
    chat::Limits limits;
    limits.max_connections_per_ip = 2;
    limits.trusted_proxies = {*net::IpNetwork::parse("127.0.0.0/8")};
    node_ = std::make_unique<Node>(GetParam(), limits);
    const std::string from_a = "X-Forwarded-For: 203.0.113.7\r\n";
    // Two upgrades from one forwarded address, both waiting on the keys.
    auto first = std::async(std::launch::async,
                            [&] { return open(from_a + "Authorization: Bearer slow.alice\r\n"); });
    auto second = std::async(std::launch::async,
                             [&] { return open(from_a + "Authorization: Bearer slow.bob\r\n"); });
    ASSERT_TRUE(ulw::test::eventually([&] { return node_->key_waiters == 2; }));
    // Told to come back as soon as an upgrade in flight is likely answered.
    const auto third = ask_upgrade(node_->port(), from_a + "Authorization: Bearer user.carol\r\n");
    EXPECT_TRUE(third.head.starts_with("HTTP/1.1 429 Too Many Requests\r\n")) << third.head;
    EXPECT_EQ(header_value(third.head, "Retry-After"), "1") << third.head;
    EXPECT_EQ(counter(R"(upgrades_limited_total{limit="ip"})"), 1U);
    // Another address, through the same proxy, is not.
    EXPECT_TRUE(open("X-Forwarded-For: 198.51.100.9\r\nAuthorization: Bearer user.carol\r\n"));
    node_->refresh_keys = true;
    auto alice = first.get();
    auto bob = second.get();
    ASSERT_TRUE(alice && bob);
    // Both open, and the address free again for its next upgrades.
    auto carol = open(from_a + "Authorization: Bearer user.carol\r\n");
    auto dave = open(from_a + "Authorization: Bearer user.dave\r\n");
    EXPECT_TRUE(carol && dave);
    // The direct peer, the proxy, was never counted as a client.
    EXPECT_EQ(counter(R"(connections_rejected_total{reason="ip_connections"})"), 0U);
}

// An upgrade refused for its token or its origin is answered, and so gives its forwarded
// address's place back at once, though the client keeps the connection open.
TEST_P(ChatSessionTest, BehindATrustedProxyAnUpgradeAnswered401Or403GivesItsAddressPlaceBack) {
    node_.reset();
    chat::Limits limits;
    limits.max_connections_per_ip = 1;
    limits.trusted_proxies = {*net::IpNetwork::parse("127.0.0.0/8")};
    node_ = std::make_unique<Node>(GetParam(), limits);
    const std::string from_a = "X-Forwarded-For: 203.0.113.7\r\n";
    std::vector<Asked> refused;
    for (const auto& [headers, status] : std::vector<std::pair<std::string, std::string>>{
             {from_a, "HTTP/1.1 401 Unauthorized"},
             {from_a + "Authorization: Bearer forged\r\n", "HTTP/1.1 401 Unauthorized"},
             {from_a + "Cookie: auth_token=user.alice\r\n", "HTTP/1.1 403 Forbidden"},
             {from_a + "Cookie: auth_token=user.alice\r\nOrigin: https://evil.test\r\n",
              "HTTP/1.1 403 Forbidden"}}) {
        refused.push_back(ask_upgrade(node_->port(), headers));
        EXPECT_TRUE(refused.back().head.starts_with(status + "\r\n")) << refused.back().head;
    }
    // Every refused connection still open: an address held until its connections close would
    // have been answered 429 from the second on, and would be now.
    EXPECT_TRUE(open(from_a + "Authorization: Bearer user.alice\r\n"));
    EXPECT_EQ(counter(R"(upgrades_limited_total{limit="ip"})"), 0U);
}

TEST_P(ChatSessionTest, PushCommandsAreAnsweredPushDisabledWhereItIsNotConfigured) {
    auto alice = open_as("alice");
    ASSERT_TRUE(alice);
    ASSERT_TRUE(alice->send_text(R"({"type":"push_key"})"));
    EXPECT_EQ(alice->next_text(seconds(10)), R"({"type":"error","reason":"push_disabled"})");
    ASSERT_TRUE(alice->send_text(
        R"({"type":"push_unsubscribe","device":"01a0eb86-6cca-7dce-84cc-3bb47615f9d1"})"));
    EXPECT_EQ(alice->next_text(seconds(10)),
              R"({"type":"error","reason":"push_disabled","device":")"
              R"(01a0eb86-6cca-7dce-84cc-3bb47615f9d1"})");
    EXPECT_EQ(counter("push_enabled"), 0U);
}

TEST_P(ChatSessionTest, ASocketRegistersAPushSubscriptionAFewTimesAMinuteAtMost) {
    node_ = std::make_unique<Node>(GetParam(), chat::Limits{}, false, true);
    ASSERT_NE(node_->port(), 0);
    auto alice = open_as("alice");
    ASSERT_TRUE(alice);
    ASSERT_TRUE(alice->send_text(R"({"type":"push_key"})"));
    const auto key = alice->next_text(seconds(10));
    ASSERT_TRUE(key.has_value());
    EXPECT_TRUE(key->starts_with(R"({"type":"push_key","key":"B)")) << *key;
    const std::string device = "01a0eb86-6cca-7dce-84cc-3bb47615f9d1";
    const std::string subscribe =
        R"({"type":"push_subscribe","device":")" + device +
        R"(","endpoint":"https://fcm.googleapis.com/fcm/send/x","p256dh":")"
        R"(BCVxsr7N_eNgVRqvHtD0zTZsEc6-VV-JvLexhqUzORcxaOzi6-AYWXvTBHm4bjyPjs7Vd8pZGH6SRpkNtoIAiw4)"
        R"(","auth":"BTBZMqHH6r4Tts7J_aSIgg"})";
    ASSERT_TRUE(alice->send_text(subscribe));
    EXPECT_EQ(alice->next_text(seconds(10)),
              R"({"type":"push_subscribed","device":")" + device + R"("})");
    ASSERT_TRUE(alice->send_text(R"({"type":"push_unsubscribe","device":")" + device + R"("})"));
    EXPECT_EQ(alice->next_text(seconds(10)),
              R"({"type":"push_unsubscribed","device":")" + device + R"("})");
    // Five at once, then one each ten seconds.
    for (int i = 0; i < 3; ++i) {
        ASSERT_TRUE(alice->send_text(subscribe));
        EXPECT_EQ(alice->next_text(seconds(10)),
                  R"({"type":"push_subscribed","device":")" + device + R"("})");
    }
    ASSERT_TRUE(alice->send_text(subscribe));
    EXPECT_EQ(alice->next_text(seconds(10)),
              R"({"type":"error","reason":"rate_limited","device":")" + device +
                  R"(","retry_after_ms":10000})");
    // A key costs the store nothing and is not counted.
    ASSERT_TRUE(alice->send_text(R"({"type":"push_key"})"));
    EXPECT_EQ(alice->next_text(seconds(10)), key);
    EXPECT_EQ(counter("push_enabled"), 1U);
    EXPECT_EQ(counter(R"(push_subscriptions_total{op="subscribed"})"), 4U);
    EXPECT_EQ(counter(R"(push_subscriptions_total{op="unsubscribed"})"), 1U);
}

INSTANTIATE_TEST_SUITE_P(Reactors, ChatSessionTest,
                         ::testing::Values(net::ReactorKind::IoUring, net::ReactorKind::Epoll),
                         ulw::test::reactor_name);

} // namespace
