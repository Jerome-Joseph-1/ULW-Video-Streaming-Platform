// The M16 acceptance: three chat_server processes on one Postgres. Clients on different nodes
// see each other's messages; a stopped owner is replaced under a higher owner_generation; once
// resumed, its stale write updates no rows, is logged as "fenced out", and reaches nobody.
// The M17 acceptance on the same cluster: clients on every node see one order by last_seq; a
// rate-limited send is refused and reaches nobody; a repeated send is delivered once; a client
// that comes back resumes from its last seq; no body shows in any log or in the database.
// ULW_CHAT_CLUSTER_PORTS=9101,9102,9103 pins the client ports (the CI job does); otherwise
// ones outside the ephemeral range are reserved.

#include "core/util/json.hpp"
#include "core/util/parse.hpp"
#include "infra/auth/base64url.hpp"
#include "os/system_clock.hpp"
#include "os/system_random.hpp"

#include "devtoken/dev_key.hpp"
#include "postgres_harness.hpp"
#include "support/child_process.hpp"
#include "support/eventually.hpp"
#include "support/reactor_harness.hpp"
#include "support/reserve_port.hpp"
#include "support/temp_dir.hpp"
#include "support/ws_client.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <format>
#include <fstream>
#include <gtest/gtest.h>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

using infra::postgres::Params;
using std::chrono::seconds;
using ulw::test::ChildProcess;
using ulw::test::ScratchDatabase;
using ulw::test::WsClient;

constexpr std::string_view kIssuer = "https://auth.test.askedin.com";

// Empty when the ports are not pinned: each node then reserves its own.
std::vector<std::uint16_t> pinned_client_ports() {
    std::vector<std::uint16_t> ports;
    // NOLINTNEXTLINE(concurrency-mt-unsafe): read before any thread starts.
    const char* pinned = std::getenv("ULW_CHAT_CLUSTER_PORTS");
    std::string_view rest = pinned == nullptr ? "" : pinned;
    while (!rest.empty()) {
        const std::size_t comma = rest.find(',');
        ports.push_back(core::parse_integer<std::uint16_t>(rest.substr(0, comma)).value_or(0));
        rest = comma == std::string_view::npos ? "" : rest.substr(comma + 1);
    }
    return ports;
}

// A server message, the fields the test looks at. The body is decoded: what the sender sent.
struct Seen {
    std::string type;
    std::uint64_t seq = 0;
    std::string sender;
    std::string id;
    std::string body;
    std::string reason;
    std::optional<std::uint64_t> retry_after_ms;
};

std::optional<Seen> parse_seen(const std::string& text) {
    const auto json = core::json::parse(text);
    if (!json) {
        return std::nullopt;
    }
    const auto string = [&](std::string_view key) {
        const core::json::Value* v = json->find(key);
        return std::string(v == nullptr ? "" : v->as_string().value_or(""));
    };
    Seen s{.type = string("type"),
           .seq = 0,
           .sender = string("sender"),
           .id = string("id"),
           .body = infra::auth::decode_base64url(string("body")).value_or("<not base64url>"),
           .reason = string("reason"),
           .retry_after_ms = std::nullopt};
    if (const core::json::Value* seq = json->find("seq")) {
        s.seq = seq->as_u64().value_or(0);
    }
    if (const core::json::Value* after = json->find("retry_after_ms")) {
        s.retry_after_ms = after->as_u64();
    }
    return s;
}

// One user's socket and everything the server has sent it.
class Client {
public:
    Client(WsClient ws, std::string name) : ws_(std::move(ws)), name_(std::move(name)) {}

    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    [[nodiscard]] const std::vector<Seen>& seen() const noexcept { return seen_; }

    bool send(const std::string& json) { return ws_.send_text(json); }

    // Reads until a message matching `pred` arrives, keeping everything read.
    template <class Pred>
    std::optional<Seen> wait_for(Pred pred, std::chrono::milliseconds limit = seconds(15)) {
        for (const Seen& s : seen_) {
            if (pred(s)) {
                return s;
            }
        }
        const auto deadline = std::chrono::steady_clock::now() + limit;
        while (std::chrono::steady_clock::now() < deadline) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());
            const auto text = ws_.next_text(left);
            if (!text) {
                return std::nullopt;
            }
            auto s = parse_seen(*text);
            if (!s) {
                ADD_FAILURE() << name_ << " got something that is not JSON: " << *text;
                return std::nullopt;
            }
            seen_.push_back(*s);
            if (pred(*s)) {
                return s;
            }
        }
        return std::nullopt;
    }

    std::optional<Seen> message(const std::string& body) {
        return wait_for([&](const Seen& s) { return s.type == "message" && s.body == body; });
    }

    [[nodiscard]] bool ever_saw(const std::string& body) const {
        return std::ranges::any_of(seen_, [&](const Seen& s) { return s.body == body; });
    }

    template <class Pred> [[nodiscard]] std::size_t count(Pred pred) const {
        return static_cast<std::size_t>(std::ranges::count_if(seen_, pred));
    }

    [[nodiscard]] std::vector<Seen> messages() const {
        std::vector<Seen> out;
        std::ranges::copy_if(seen_, std::back_inserter(out),
                             [](const Seen& s) { return s.type == "message"; });
        return out;
    }

private:
    WsClient ws_;
    std::string name_;
    std::vector<Seen> seen_;
};

struct Node {
    std::string name;
    std::uint16_t port = 0;
    bool port_pinned = false;
    std::uint16_t node_port = 0;
    std::unique_ptr<ChildProcess> process;
};

class ChatClusterTest : public ::testing::TestWithParam<net::ReactorKind> {
protected:
    void SetUp() override {
        ScratchDatabase::open(db_);
        if (IsSkipped() || HasFatalFailure()) {
            return;
        }
        room_ = core::RoomId::generate(clock_, random_).to_string();
        // Made up per run: no real secret lives in the repository.
        std::array<std::byte, 32> secret{};
        random_.fill(secret);
        for (const std::byte b : secret) {
            node_secret_ += std::format("{:02x}", std::to_integer<unsigned>(b));
        }
        auto key = devtoken::DevKey::generate();
        ASSERT_TRUE(key);
        const auto jwks = files_.path() / "jwks.json";
        std::ofstream(jwks) << key->public_jwks();
        for (const char* user : {"alice", "bob", "carol"}) {
            tokens_.push_back(*key->mint({.issuer = std::string(kIssuer),
                                          .audience = "askedin-platform",
                                          .subject = user,
                                          .email = {},
                                          .ttl = seconds(600)},
                                         clock_.wall_now()));
        }
        const auto pinned = pinned_client_ports();
        for (std::size_t i = 0; i < 3; ++i) {
            nodes_.push_back({.name = "chat-" + std::to_string(i + 1),
                              .port = i < pinned.size() ? pinned[i] : std::uint16_t{0},
                              .port_pinned = i < pinned.size(),
                              .node_port = 0,
                              .process = nullptr});
            ASSERT_NO_FATAL_FAILURE(start(nodes_.back(), jwks.string()));
        }
        for (const Node& n : nodes_) {
            ASSERT_TRUE(ulw::test::eventually(
                [&] { return ulw::test::http_get(n.port, "/readyz").status == 200; }, seconds(30)))
                << n.process->output();
        }
    }

    void start(Node& node, const std::string& jwks) {
        std::vector<std::string> env{
            "ULW_NODE_ID=" + node.name, "ULW_DEV_LOOPBACK_NODES=1",
            "ULW_NODE_SECRET=" + node_secret_, "ULW_DATABASE_URL=" + db_->conninfo(),
            "ULW_DEV_JWKS_FILE=" + jwks, "JWT_ISSUER=" + std::string(kIssuer),
            "ULW_REACTOR=" +
                std::string(GetParam() == net::ReactorKind::IoUring ? "io_uring" : "epoll"),
            // Some runs start tests as root; this suite is not about that.
            "ULW_ALLOW_ROOT=1"};
        for (const char* passed : {"ASAN_OPTIONS", "UBSAN_OPTIONS", "LSAN_OPTIONS"}) {
            // NOLINTNEXTLINE(concurrency-mt-unsafe): read before any thread starts.
            if (const char* value = std::getenv(passed)) {
                env.push_back(std::string(passed) + "=" + value);
            }
        }
        auto started = ulw::test::start_until_listening(
            [&] {
                if (!node.port_pinned) {
                    node.port = ulw::test::reserve_port();
                }
                node.node_port = ulw::test::reserve_port();
                if (node.port == 0 || node.node_port == 0) {
                    return std::unique_ptr<ChildProcess>();
                }
                auto with_ports = env;
                with_ports.push_back("ULW_LISTEN_PORT=" + std::to_string(node.port));
                with_ports.push_back("ULW_NODE_ADDRESS=127.0.0.1:" +
                                     std::to_string(node.node_port));
                return ChildProcess::start({ULW_CHAT_BIN}, with_ports);
            },
            R"("msg":"listening")", seconds(30));
        node.process = std::move(started.process);
        ASSERT_NE(node.process, nullptr) << "no port to listen on";
        ASSERT_TRUE(started.ready) << node.process->output();
    }

    std::unique_ptr<Client> connect(const Node& node, std::size_t user) {
        std::string refusal;
        auto ws = WsClient::connect(node.port, "/rt",
                                    "Authorization: Bearer " + tokens_[user] + "\r\n", &refusal);
        if (!ws) {
            ADD_FAILURE() << "upgrade refused: " << refusal;
            return nullptr;
        }
        return std::make_unique<Client>(std::move(*ws),
                                        std::array{"alice", "bob", "carol"}.at(user));
    }

    void join(Client& client, std::optional<std::uint64_t> after = std::nullopt) {
        ASSERT_TRUE(client.send(R"({"type":"join","room":")" + room_ + R"(")" +
                                (after ? R"(,"after":)" + std::to_string(*after) : "") + "}"));
        const auto joined =
            client.wait_for([](const Seen& s) { return s.type == "joined" || s.type == "error"; });
        ASSERT_TRUE(joined);
        ASSERT_EQ(joined->type, "joined") << joined->reason;
    }

    static std::string send_command(const std::string& room, const std::string& body,
                                    const std::string& id) {
        return R"({"type":"send","room":")" + room + R"(","id":")" + id + R"(","body":")" +
               infra::auth::encode_base64url(body) + R"("})";
    }

    static std::string ref(std::uint64_t n) { return "r" + std::to_string(n); }

    // Sends until the client gets its own message back: then its node routes to the room's
    // current owner and receives what that owner sequences. Returns the message's seq.
    std::optional<std::uint64_t> send_until_heard(Client& client, const std::string& body) {
        for (int attempt = 0; attempt < 10; ++attempt) {
            const std::string id = ref(next_ref_++);
            const std::string text = body + " #" + std::to_string(attempt);
            if (!client.send(send_command(room_, text, id))) {
                return std::nullopt;
            }
            const auto outcome = client.wait_for([&](const Seen& s) {
                return (s.type == "message" && s.body == text) || (s.type == "error" && s.id == id);
            });
            if (outcome && outcome->type == "message") {
                last_body_[client.name()] = text;
                return outcome->seq;
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] std::string owner() const {
        auto conn = db_->session();
        return ulw::test::scalar(conn,
                                 "SELECT owner_node || ' ' || owner_generation "
                                 "FROM room_assignments WHERE room_id = $1::text::uuid",
                                 Params{}.add_text(room_));
    }

    [[nodiscard]] std::string last_seq() const {
        auto conn = db_->session();
        return ulw::test::scalar(conn,
                                 "SELECT last_seq FROM room_state WHERE room_id = $1::text::uuid",
                                 Params{}.add_text(room_));
    }

    // Every row of every table, as text; bytea shows as hex.
    [[nodiscard]] std::string database_text() const {
        auto conn = db_->session();
        return ulw::test::scalar(conn, R"sql(
SELECT string_agg(query_to_xml(format('SELECT t::text AS row FROM %I.%I t', table_schema,
                                      table_name), false, false, '')::text, E'\n')
  FROM information_schema.tables
 WHERE table_type = 'BASE TABLE' AND table_schema NOT IN ('pg_catalog', 'information_schema'))sql");
    }

    [[nodiscard]] static std::uint64_t metric(const Node& node, std::string_view name) {
        const std::string body = ulw::test::http_get(node.port, "/metrics").body;
        const std::string line = std::string(name) + " ";
        const std::size_t at = body.find(line);
        if (at == std::string::npos || (at > 0 && body[at - 1] != '\n')) {
            ADD_FAILURE() << name << " is not in the metrics";
            return 0;
        }
        const std::size_t start = at + line.size();
        return core::parse_integer<std::uint64_t>(
                   std::string_view(body).substr(start, body.find('\n', start) - start))
            .value_or(0);
    }

    // Section 8.15: bodies are never logged, indexed or stored as anything a grep could read.
    // The database is searched for each body as it was sent, as the client encoded it, and as
    // bytea would show it. Until messages are stored (M19) there are no rows a body could be
    // in, so only the logs half can fail; from then on this must also check that the rows grew
    // by the stored bodies, or it proves nothing about them.
    void expect_no_plaintext(const std::vector<std::string>& bodies) const {
        const std::string database = database_text();
        ASSERT_NE(database.find(room_), std::string::npos) << "no rows read";
        for (const std::string& body : bodies) {
            std::string hex;
            for (const char c : body) {
                hex += std::format("{:02x}", static_cast<unsigned char>(c));
            }
            for (const std::string& form : {body, infra::auth::encode_base64url(body), hex}) {
                EXPECT_EQ(database.find(form), std::string::npos) << "the database holds " << form;
                for (const Node& n : nodes_) {
                    EXPECT_EQ(n.process->output().find(form), std::string::npos)
                        << n.name << " logged " << form;
                }
            }
        }
        if (!HasFailure()) {
            std::cout << "searched " << database.size() << " bytes of rows and " << nodes_.size()
                      << " logs for " << bodies.size() << (bodies.size() == 1 ? " body" : " bodies")
                      << " in three forms: none found\n";
        }
    }

    os::SystemClock clock_;
    os::SystemRandom random_;
    std::unique_ptr<ScratchDatabase> db_;
    ulw::test::TempDir files_{"ulw-chat-cluster"};
    std::vector<std::string> tokens_;
    std::vector<Node> nodes_;
    std::string room_;
    std::string node_secret_;
    std::uint64_t next_ref_ = 100;
    std::unordered_map<std::string, std::string> last_body_;
};

TEST_P(ChatClusterTest, AStoppedOwnerIsReplacedAndItsLateWriteIsFencedOutAndDeliveredNowhere) {
    auto alice = connect(nodes_[0], 0);
    auto bob = connect(nodes_[1], 1);
    auto carol = connect(nodes_[2], 2);
    ASSERT_TRUE(alice && bob && carol);
    // chat-1 resolves the room first, and so creates it and owns it.
    ASSERT_NO_FATAL_FAILURE(join(*alice));
    ASSERT_NO_FATAL_FAILURE(join(*bob));
    ASSERT_NO_FATAL_FAILURE(join(*carol));
    ASSERT_EQ(owner(), "chat-1 1");

    // Clients on different nodes see each other's messages, each under the one seq its owner
    // gave it.
    ASSERT_TRUE(bob->send(send_command(room_, "hello from chat-2", ref(1))));
    ASSERT_TRUE(alice->send(send_command(room_, "hello from chat-1", ref(2))));
    std::optional<std::uint64_t> from_bob;
    std::optional<std::uint64_t> from_alice;
    for (Client* c : {alice.get(), bob.get(), carol.get()}) {
        const auto first = c->message("hello from chat-2");
        const auto second = c->message("hello from chat-1");
        ASSERT_TRUE(first && second) << c->name();
        EXPECT_EQ(first->sender, "bob");
        EXPECT_EQ(second->sender, "alice");
        EXPECT_EQ(first->seq, from_bob.value_or(first->seq)) << c->name();
        EXPECT_EQ(second->seq, from_alice.value_or(second->seq)) << c->name();
        from_bob = first->seq;
        from_alice = second->seq;
    }
    EXPECT_EQ(*from_bob + *from_alice, 3U) << "the two messages are seqs 1 and 2";

    // Stop the owner, and give it a write to make once it runs again.
    auto watch = db_->session();
    ASSERT_TRUE(watch.exec("LISTEN room_owner"));
    Node& stalled = nodes_[0];
    stalled.process->signal(SIGSTOP);
    ASSERT_TRUE(alice->send(send_command(room_, "stale", ref(7))));

    // A survivor claims the room under a higher generation.
    const auto deadline = std::chrono::steady_clock::now() + seconds(30);
    std::string now_owned = owner();
    while (now_owned == "chat-1 1" && std::chrono::steady_clock::now() < deadline) {
        ASSERT_TRUE(watch.wait_for_notification(core::Millis{1'000}));
        now_owned = owner();
    }
    ASSERT_TRUE(now_owned == "chat-2 2" || now_owned == "chat-3 2") << now_owned;
    std::cout << "owner before the stop: chat-1 1, after: " << now_owned << "\n";

    // The survivors carry on through the new owner.
    ASSERT_TRUE(send_until_heard(*bob, "bob after the failover"));
    ASSERT_TRUE(send_until_heard(*carol, "carol after the failover"));
    ASSERT_TRUE(bob->message(last_body_["carol"]));
    ASSERT_TRUE(carol->message(last_body_["bob"]));
    const std::string seq_before = last_seq();

    // Resumed, the old owner still believes it owns the room: its write is fenced out.
    stalled.process->signal(SIGCONT);
    const std::string fenced = R"("msg":"fenced out","node":"chat-1","room":")" + room_ +
                               R"(","generation":1,"write":"append"})";
    ASSERT_TRUE(stalled.process->wait_for_output(fenced, seconds(30))) << stalled.process->output();
    const auto refused = alice->wait_for([](const Seen& s) { return s.id == ref(7); });
    ASSERT_TRUE(refused);
    EXPECT_EQ(refused->type, "error");
    // "fenced" when the append's own answer comes back first; "unavailable" when the resumed
    // node's heartbeat finds the fence while the append is still out, and its fate is unknown
    // to the node. Either way the client is told it was not sequenced for certain.
    EXPECT_TRUE(refused->reason == "fenced" || refused->reason == "unavailable") << refused->reason;
    EXPECT_EQ(last_seq(), seq_before) << "the stale write updated a row";
    const std::string& logged = stalled.process->output();
    const std::size_t at = logged.find(fenced);
    const std::size_t line = logged.rfind('\n', at) + 1;
    std::cout << "chat-1 logged: " << logged.substr(line, logged.find('\n', at) - line) << "\n";

    // The old owner's node routes to the new owner from here on, and everyone hears it.
    const auto back = send_until_heard(*alice, "alice is back");
    ASSERT_TRUE(back);
    for (Client* c : {bob.get(), carol.get()}) {
        const auto heard = c->message(last_body_["alice"]);
        ASSERT_TRUE(heard) << c->name();
        EXPECT_EQ(heard->seq, *back);
    }
    for (Client* c : {alice.get(), bob.get(), carol.get()}) {
        EXPECT_FALSE(c->ever_saw("stale")) << c->name() << " was delivered the fenced write";
    }
    EXPECT_NE(ulw::test::http_get(stalled.port, "/metrics").body.find("fenced_writes_total "),
              std::string::npos);
    EXPECT_EQ(ulw::test::http_get(stalled.port, "/metrics").body.find("fenced_writes_total 0\n"),
              std::string::npos);

    // Every node drains cleanly, the resumed one included.
    for (Node& n : nodes_) {
        n.process->signal(SIGTERM);
    }
    for (Node& n : nodes_) {
        EXPECT_EQ(n.process->wait_exit(seconds(30)), 0) << n.name << "\n" << n.process->output();
    }

    // Each ran on the reactor it was asked for, not a fallback.
    const std::string reactor = GetParam() == net::ReactorKind::IoUring ? R"("reactor":"io_uring")"
                                                                        : R"("reactor":"epoll")";
    // Bodies, tokens and the node secret never reach a log (section 8.14).
    std::vector<std::string> secrets = tokens_;
    secrets.push_back(node_secret_);
    for (const std::string body :
         {"hello from chat-2", "hello from chat-1", "stale", "bob after the failover",
          "carol after the failover", "alice is back"}) {
        secrets.push_back(body);
        secrets.push_back(infra::auth::encode_base64url(body));
    }
    for (const Node& n : nodes_) {
        const std::string& output = n.process->output();
        EXPECT_NE(output.find(reactor + ","), std::string::npos) << n.name << "\n" << output;
        for (const std::string& secret : secrets) {
            EXPECT_EQ(output.find(secret), std::string::npos) << n.name << " logged " << secret;
        }
    }
}

TEST_P(ChatClusterTest, ClientsOnEveryNodeSeeEveryMessageInOneOrderByLastSeq) {
    // Two clients on each node, as three users, each user on two nodes.
    std::vector<std::unique_ptr<Client>> clients;
    for (std::size_t node = 0; node < nodes_.size(); ++node) {
        for (std::size_t k = 0; k < 2; ++k) {
            clients.push_back(connect(nodes_[node], (node + k) % 3));
            ASSERT_TRUE(clients.back());
            ASSERT_NO_FATAL_FAILURE(join(*clients.back()));
        }
    }
    // Every client sends all its messages at once, without waiting for an answer: the owner
    // sees them interleaved from three nodes.
    constexpr int kEach = 4;
    std::vector<std::string> bodies;
    for (int round = 0; round < kEach; ++round) {
        for (std::size_t i = 0; i < clients.size(); ++i) {
            bodies.push_back(std::format("order check {} from client {}", round, i));
            ASSERT_TRUE(clients[i]->send(
                send_command(room_, bodies.back(), std::format("c{}-{}", i, round))));
        }
    }
    const std::size_t total = bodies.size();
    for (auto& c : clients) {
        ASSERT_TRUE(c->wait_for([&](const Seen&) { return c->messages().size() == total; }))
            << c->name() << " heard " << c->messages().size() << " of " << total;
    }

    const std::vector<Seen> first = clients[0]->messages();
    for (std::size_t k = 0; k < first.size(); ++k) {
        EXPECT_EQ(first[k].seq, first[0].seq + k) << "seqs run without gaps";
    }
    for (auto& c : clients) {
        const std::vector<Seen> heard = c->messages();
        for (std::size_t k = 0; k < total; ++k) {
            EXPECT_EQ(heard[k].seq, first[k].seq) << c->name();
            EXPECT_EQ(heard[k].id, first[k].id) << c->name();
            EXPECT_EQ(heard[k].body, first[k].body) << c->name();
        }
    }
    for (const std::string& body : bodies) {
        EXPECT_EQ(std::ranges::count(first, body, &Seen::body), 1) << body;
    }
    // Each sender's own ack names the seq everyone saw its message under.
    for (std::size_t i = 0; i < clients.size(); ++i) {
        for (int round = 0; round < kEach; ++round) {
            const std::string id = std::format("c{}-{}", i, round);
            const auto ack =
                clients[i]->wait_for([&](const Seen& s) { return s.type == "sent" && s.id == id; });
            ASSERT_TRUE(ack) << id;
            const auto it = std::ranges::find(first, id, &Seen::id);
            ASSERT_NE(it, first.end());
            EXPECT_EQ(ack->seq, it->seq) << id;
        }
    }
    EXPECT_EQ(last_seq(), std::to_string(first.back().seq));
    std::cout << clients.size() << " clients on " << nodes_.size() << " nodes saw seqs "
              << first.front().seq << ".." << first.back().seq
              << " in one order; last_seq in room_state is " << last_seq() << "\n";
    ASSERT_NO_FATAL_FAILURE(expect_no_plaintext(bodies));
}

TEST_P(ChatClusterTest, ARateLimitedSendIsRefusedAndReachesNobody) {
    auto alice = connect(nodes_[0], 0);
    auto bob = connect(nodes_[1], 1);
    auto carol = connect(nodes_[2], 2);
    ASSERT_TRUE(alice && bob && carol);
    for (Client* c : {alice.get(), bob.get(), carol.get()}) {
        ASSERT_NO_FATAL_FAILURE(join(*c));
    }
    // Sixteen at once against a burst of ten refilling at two a second: all but ten are
    // refused unless the node took seconds to read them.
    constexpr std::size_t kBurst = 16;
    std::vector<std::string> bodies;
    for (std::size_t i = 0; i < kBurst; ++i) {
        bodies.push_back(std::format("burst message {}", i));
        ASSERT_TRUE(bob->send(send_command(room_, bodies.back(), std::format("b{}", i))));
    }
    std::vector<std::string> limited;
    std::uint64_t retry_after_ms = 0;
    for (std::size_t i = 0; i < kBurst; ++i) {
        const std::string id = std::format("b{}", i);
        const auto answer = bob->wait_for(
            [&](const Seen& s) { return (s.type == "sent" || s.type == "error") && s.id == id; });
        ASSERT_TRUE(answer) << id;
        if (answer->type == "error") {
            EXPECT_EQ(answer->reason, "rate_limited") << id;
            ASSERT_TRUE(answer->retry_after_ms) << id;
            retry_after_ms = std::max(retry_after_ms, *answer->retry_after_ms);
            limited.push_back(id);
        }
    }
    ASSERT_GE(limited.size(), 1U);
    ASSERT_LE(limited.size(), kBurst - 10) << "at most those past the burst";

    // bob tries a marker until one is taken, each try once the last is answered: no clock in
    // the test, only the server's answers. Anything refused would have been sequenced before
    // the marker, on the same connection, so its absence below is final.
    std::size_t marker_refusals = 0;
    for (;;) {
        const std::string id = std::format("marker-{}", marker_refusals);
        ASSERT_TRUE(bob->send(send_command(room_, "after the limit", id)));
        const auto answer = bob->wait_for(
            [&](const Seen& s) { return (s.type == "sent" || s.type == "error") && s.id == id; });
        ASSERT_TRUE(answer) << id;
        if (answer->type == "sent") {
            break;
        }
        ASSERT_EQ(answer->reason, "rate_limited");
        // 2 a second: the bucket refills within half a second, far fewer tries than this.
        ASSERT_LT(++marker_refusals, 100'000U) << "the bucket never refilled";
    }
    for (Client* c : {alice.get(), bob.get(), carol.get()}) {
        ASSERT_TRUE(c->message("after the limit")) << c->name();
        EXPECT_EQ(c->count([](const Seen& s) {
            return s.type == "message" && s.body.starts_with("burst message");
        }),
                  kBurst - limited.size())
            << c->name();
        for (const std::string& id : limited) {
            EXPECT_EQ(c->count([&](const Seen& s) { return s.type == "message" && s.id == id; }),
                      0U)
                << c->name() << " was delivered " << id;
        }
    }
    EXPECT_EQ(metric(nodes_[1], "messages_rate_limited_total"), limited.size() + marker_refusals);
    std::cout << "bob sent " << kBurst << " at once: " << limited.size()
              << " refused as rate_limited (retry after " << retry_after_ms
              << " ms), delivered to nobody; the rest reached all three nodes\n";
    ASSERT_NO_FATAL_FAILURE(expect_no_plaintext(bodies));
}

TEST_P(ChatClusterTest, ASendRepeatedWithItsIdIsDeliveredOnceWhereverItIsRepeated) {
    auto alice = connect(nodes_[0], 0);
    auto bob = connect(nodes_[1], 1);
    ASSERT_TRUE(alice && bob);
    ASSERT_NO_FATAL_FAILURE(join(*alice));
    ASSERT_NO_FATAL_FAILURE(join(*bob));
    const std::string body = "said once, sent three times";
    const auto acks = [&](Client& c) {
        return c.count([](const Seen& s) { return s.type == "sent" && s.id == "retry-1"; });
    };
    ASSERT_TRUE(alice->send(send_command(room_, body, "retry-1")));
    const auto ack =
        alice->wait_for([](const Seen& s) { return s.type == "sent" && s.id == "retry-1"; });
    ASSERT_TRUE(ack);
    const std::uint64_t seq = ack->seq;
    // Again on the same connection, as after `unavailable`...
    ASSERT_TRUE(alice->send(send_command(room_, body, "retry-1")));
    ASSERT_TRUE(alice->wait_for([&](const Seen&) { return acks(*alice) == 2; }));
    // ...and from a connection on another node, as after a reconnect that landed elsewhere.
    ASSERT_TRUE(bob->message(body));
    auto elsewhere = connect(nodes_[1], 0);
    ASSERT_TRUE(elsewhere);
    ASSERT_NO_FATAL_FAILURE(join(*elsewhere));
    ASSERT_TRUE(elsewhere->send(send_command(room_, body, "retry-1")));
    const auto again =
        elsewhere->wait_for([](const Seen& s) { return s.type == "sent" && s.id == "retry-1"; });
    ASSERT_TRUE(again);
    EXPECT_EQ(again->seq, seq);
    for (const Seen& s : alice->messages()) {
        EXPECT_TRUE(s.id != "retry-1" || s.seq == seq);
    }

    ASSERT_TRUE(alice->send(send_command(room_, "after the retries", "marker")));
    for (Client* c : {alice.get(), bob.get(), elsewhere.get()}) {
        ASSERT_TRUE(c->message("after the retries")) << c->name();
    }
    EXPECT_EQ(alice->count([&](const Seen& s) { return s.body == body; }), 1U);
    EXPECT_EQ(bob->count([&](const Seen& s) { return s.body == body; }), 1U);
    EXPECT_EQ(elsewhere->count([&](const Seen& s) { return s.body == body; }), 0U);
    EXPECT_EQ(last_seq(), std::to_string(seq + 1));
    EXPECT_EQ(metric(nodes_[0], "messages_deduplicated_total") +
                  metric(nodes_[1], "messages_deduplicated_total"),
              2U);
    std::cout << "one message sent three times, twice on chat-1 and once on chat-2: acked as seq "
              << seq << " each time, delivered once\n";
    ASSERT_NO_FATAL_FAILURE(expect_no_plaintext({body}));
}

TEST_P(ChatClusterTest, AClientThatComesBackResumesFromItsLastSeq) {
    auto alice = connect(nodes_[0], 0);
    auto bob = connect(nodes_[1], 1);
    ASSERT_TRUE(alice && bob);
    ASSERT_NO_FATAL_FAILURE(join(*alice));
    ASSERT_NO_FATAL_FAILURE(join(*bob));
    const auto last = send_until_heard(*alice, "before bob left");
    ASSERT_TRUE(last);
    ASSERT_TRUE(bob->message(last_body_["alice"]));
    // bob was chat-2's only client in the room.
    bob.reset();

    std::vector<std::string> missed;
    for (int i = 0; i < 3; ++i) {
        missed.push_back(std::format("while bob was away {}", i));
        ASSERT_TRUE(alice->send(send_command(room_, missed.back(), std::format("away-{}", i))));
        ASSERT_TRUE(alice->message(missed.back()));
    }
    auto back = connect(nodes_[1], 1);
    ASSERT_TRUE(back);
    ASSERT_NO_FATAL_FAILURE(join(*back, last));
    std::vector<std::uint64_t> seqs;
    for (const std::string& body : missed) {
        const auto m = back->message(body);
        ASSERT_TRUE(m) << body;
        seqs.push_back(m->seq);
    }
    EXPECT_EQ(seqs, (std::vector<std::uint64_t>{*last + 1, *last + 2, *last + 3}));
    EXPECT_EQ(back->messages().size(), 3U) << "nothing at or before the seq it named";
    ASSERT_TRUE(alice->send(send_command(room_, "welcome back", "live")));
    const auto live = back->message("welcome back");
    ASSERT_TRUE(live);
    EXPECT_EQ(live->seq, *last + 4);
    EXPECT_EQ(metric(nodes_[1], "messages_replayed_total"), 3U);
    std::cout << "bob came back to chat-2 after seq " << *last << " and was sent seqs "
              << seqs.front() << ".." << seqs.back() << " before the live seq " << live->seq
              << "\n";
    missed.emplace_back("welcome back");
    ASSERT_NO_FATAL_FAILURE(expect_no_plaintext(missed));
}

TEST_P(ChatClusterTest, AClientResumingThroughANodeThatKeptNothingLearnsWhatItMissed) {
    auto alice = connect(nodes_[0], 0);
    auto bob = connect(nodes_[1], 1);
    ASSERT_TRUE(alice && bob);
    ASSERT_NO_FATAL_FAILURE(join(*alice));
    ASSERT_NO_FATAL_FAILURE(join(*bob));
    const auto last = send_until_heard(*alice, "before bob left");
    ASSERT_TRUE(last);
    ASSERT_TRUE(bob->message(last_body_["alice"]));
    bob.reset();
    std::vector<std::string> missed;
    for (int i = 0; i < 3; ++i) {
        missed.push_back(std::format("sent while bob was elsewhere {}", i));
        ASSERT_TRUE(alice->send(send_command(room_, missed.back(), std::format("gone-{}", i))));
        ASSERT_TRUE(alice->message(missed.back()));
    }
    // chat-3 was never in the room: it has nothing to send again, only the head to name.
    auto elsewhere = connect(nodes_[2], 1);
    ASSERT_TRUE(elsewhere);
    ASSERT_NO_FATAL_FAILURE(join(*elsewhere, last));
    const auto joined = elsewhere->wait_for([](const Seen& s) { return s.type == "joined"; });
    ASSERT_TRUE(joined);
    EXPECT_EQ(joined->seq, *last + 3) << "the client can tell it missed three messages";
    EXPECT_TRUE(elsewhere->messages().empty());
    std::cout << "bob came back through chat-3 after seq " << *last
              << ", which kept nothing; joined named the head " << joined->seq << "\n";
    missed.emplace_back("before bob left");
    ASSERT_NO_FATAL_FAILURE(expect_no_plaintext(missed));
}

// No database is reached: the connection string is refused before any connection is tried.
TEST(ChatServerStartup, ARefusedDatabaseUrlIsNeverEchoedBecauseItHoldsThePassword) {
    for (const std::string url :
         {"postgres://ulw:hunt%zzer2@db/ulw", "host=db password=hunt%zzer2 dbname='ulw"}) {
        auto chat = ChildProcess::start({ULW_CHAT_BIN},
                                        {"ULW_NODE_ID=chat-1", "ULW_NODE_ADDRESS=127.0.0.1:9201",
                                         "ULW_NODE_SECRET=startup-test-node-secret-000000000000",
                                         "ULW_DEV_LOOPBACK_NODES=1", "ULW_DATABASE_URL=" + url,
                                         "ULW_DEV_JWKS_FILE=/nonexistent/jwks.json",
                                         "JWT_ISSUER=https://issuer.test", "ULW_ALLOW_ROOT=1"});
        ASSERT_NE(chat, nullptr);
        EXPECT_EQ(chat->wait_exit(seconds(30)), 2) << chat->output();
        EXPECT_NE(chat->output().find("ULW_DATABASE_URL"), std::string::npos) << chat->output();
        EXPECT_EQ(chat->output().find("hunt"), std::string::npos) << chat->output();
    }
}

INSTANTIATE_TEST_SUITE_P(Reactors, ChatClusterTest,
                         ::testing::Values(net::ReactorKind::IoUring, net::ReactorKind::Epoll),
                         ulw::test::reactor_name);

} // namespace
