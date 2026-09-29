// The M16 acceptance: three chat_server processes on one Postgres. Clients on different nodes
// see each other's messages; a stopped owner is replaced under a higher owner_generation; once
// resumed, its stale write updates no rows, is logged as "fenced out", and reaches nobody.
// The M17 acceptance on the same cluster: clients on every node see one order by last_seq; a
// rate-limited send is refused and reaches nobody; a repeated send is delivered once; a client
// that comes back resumes from its last seq; no body shows in any log or in the database.
// The M19 acceptance: every message is stored with its seq, as the bytes that were sent and in
// no readable form besides; history survives a restart of every node, in order; a client that
// resumes through a node that kept nothing fills the gap from history; a room with members
// refuses anyone else.
// The M32 acceptance: a stream's live chat on the same cluster. A viewer that all but stops
// reading holds its node to no more memory and is counted as dropping; every other viewer, on
// every node, gets every message in order.
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
#include <span>
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
constexpr std::chrono::milliseconds kReadyCheckPeriod{250};

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
    std::string room;
    std::uint64_t seq = 0;
    std::string sender;
    std::string id;
    std::string body;
    std::string reason;
    std::optional<std::uint64_t> retry_after_ms;
    // Of a history answer: how many messages came before it.
    std::uint64_t count = 0;
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
           .room = string("room"),
           .seq = 0,
           .sender = string("sender"),
           .id = string("id"),
           .body = infra::auth::decode_base64url(string("body")).value_or("<not base64url>"),
           .reason = string("reason"),
           .retry_after_ms = std::nullopt,
           .count = 0};
    if (const core::json::Value* count = json->find("count")) {
        s.count = count->as_u64().value_or(0);
    }
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
    std::size_t trickle(std::size_t bytes) { return ws_.read_at_most(bytes); }

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
        // The room the tests share is a group chat of the three users.
        ASSERT_NO_FATAL_FAILURE(list_members(room_, {"alice", "bob", "carol"}));
        // Made up per run: no real secret lives in the repository.
        std::array<std::byte, 32> secret{};
        random_.fill(secret);
        for (const std::byte b : secret) {
            node_secret_ += std::format("{:02x}", std::to_integer<unsigned>(b));
        }
        auto key = devtoken::DevKey::generate();
        ASSERT_TRUE(key);
        key_.emplace(std::move(*key));
        const auto jwks = files_.path() / "jwks.json";
        std::ofstream(jwks) << key_->public_jwks();
        jwks_ = jwks.string();
        for (const char* user : {"alice", "bob", "carol"}) {
            tokens_.push_back(mint(user));
        }
        const auto pinned = pinned_client_ports();
        for (std::size_t i = 0; i < 3; ++i) {
            nodes_.push_back({.name = "chat-" + std::to_string(i + 1),
                              .port = i < pinned.size() ? pinned[i] : std::uint16_t{0},
                              .port_pinned = i < pinned.size(),
                              .node_port = 0,
                              .process = nullptr});
            ASSERT_NO_FATAL_FAILURE(start(nodes_.back(), jwks_));
        }
        ASSERT_NO_FATAL_FAILURE(wait_ready());
    }

    void wait_ready() {
        // Readiness is only visible over HTTP, so it is asked a few times a second rather than
        // in a loop that would leave a connection in TIME_WAIT on every turn.
        for (const Node& n : nodes_) {
            ASSERT_TRUE(n.process->poll_until(
                [&] { return ulw::test::http_get(n.port, "/readyz").status == 200; }, seconds(30),
                kReadyCheckPeriod))
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

    [[nodiscard]] std::string mint(const std::string& user) const {
        return key_
            ->mint({.issuer = std::string(kIssuer),
                    .audience = "askedin-platform",
                    .subject = user,
                    .email = {},
                    .ttl = seconds(600)},
                   clock_.wall_now())
            .value_or("");
    }

    std::unique_ptr<Client> connect_as(const Node& node, const std::string& user,
                                       int receive_buffer = 0) {
        std::string refusal;
        auto ws =
            WsClient::connect(node.port, "/rt", "Authorization: Bearer " + mint(user) + "\r\n",
                              &refusal, receive_buffer);
        if (!ws) {
            ADD_FAILURE() << "upgrade refused: " << refusal;
            return nullptr;
        }
        return std::make_unique<Client>(std::move(*ws), user);
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

    // Lists members for a room, as the service's operators do (RUNBOOK section 3): the room is
    // recorded closed first.
    void list_members(const std::string& room, const std::vector<std::string>& users) const {
        auto conn = db_->session();
        ASSERT_TRUE(conn.exec("INSERT INTO chat_rooms (room_id, kind) "
                              "VALUES ($1::text::uuid, 'group_chat') "
                              "ON CONFLICT (room_id) DO NOTHING",
                              Params{}.add_text(room)));
        for (const std::string& user : users) {
            ASSERT_TRUE(
                conn.exec("INSERT INTO chat_members (room_id, user_id) VALUES ($1::text::uuid, $2)",
                          Params{}.add_text(room).add_text(user)));
        }
    }

    // Opens a stream's live chat, as the server side does (RUNBOOK section 3); the kind its
    // room is then recorded as.
    [[nodiscard]] std::string record_live(const std::string& stream) const {
        auto conn = db_->session();
        return ulw::test::scalar(
            conn,
            "INSERT INTO chat_rooms (room_id, kind) "
            "SELECT live_chat_room($1), 'stream_live_chat' WHERE NOT EXISTS "
            "(SELECT 1 FROM chat_members WHERE room_id = live_chat_room($1)) "
            "AND NOT EXISTS (SELECT 1 FROM room_state WHERE room_id = live_chat_room($1) "
            "AND kind <> 'stream_live_chat') "
            "ON CONFLICT (room_id) DO UPDATE SET kind = chat_rooms.kind RETURNING kind",
            Params{}.add_text(stream));
    }

    // A join of a stream's live chat; "joined", or the error's reason.
    static std::string stream_join_answer(Client& client, const std::string& stream) {
        if (!client.send(R"({"type":"join","stream":")" + stream + R"("})")) {
            return "not sent";
        }
        const auto answer =
            client.wait_for([](const Seen& s) { return s.type == "joined" || s.type == "error"; });
        if (!answer) {
            return "no answer";
        }
        return answer->type == "joined" ? "joined" : answer->reason;
    }

    // A join of `room` with `fields` added; "joined", or the error's reason.
    static std::string join_answer(Client& client, const std::string& room,
                                   const std::string& fields = "") {
        if (!client.send(R"({"type":"join","room":")" + room + R"(")" + fields + "}")) {
            return "not sent";
        }
        const auto answer = client.wait_for([](const Seen& s) {
            return s.type == "joined" || (s.type == "error" && s.reason != "not_joined");
        });
        if (!answer) {
            return "no answer";
        }
        return answer->type == "joined" ? "joined" : answer->reason;
    }

    // Asks for a page of the room's history with `fields` (`,"after":7`, say) and returns the
    // messages that came before the page's end, as sent; nullopt for an error instead.
    std::optional<std::vector<Seen>> history(Client& client, const std::string& fields) {
        const auto is_end = [](const Seen& s) { return s.type == "history" || s.type == "error"; };
        const std::size_t ends = client.count(is_end);
        const std::size_t from = client.seen().size();
        if (!client.send(R"({"type":"history","room":")" + room_ + R"(")" + fields + "}")) {
            return std::nullopt;
        }
        const auto end = client.wait_for([&](const Seen&) { return client.count(is_end) > ends; });
        if (!end || end->type != "history") {
            ADD_FAILURE() << "history: " << (end ? end->reason : "no answer");
            return std::nullopt;
        }
        std::vector<Seen> page;
        for (std::size_t i = from; i < client.seen().size(); ++i) {
            if (client.seen()[i].type == "message") {
                page.push_back(client.seen()[i]);
            }
        }
        EXPECT_EQ(page.size(), end->count);
        return page;
    }

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

    // Every row of every table, as text, but for the bytes of stored bodies: those are the
    // messages themselves, and are checked apart (stored_exactly). Their length stands in.
    [[nodiscard]] std::string database_text() const {
        auto conn = db_->session();
        return ulw::test::scalar(conn, R"sql(
SELECT string_agg(query_to_xml(format('SELECT t::text AS row FROM %I.%I t', table_schema,
                                      table_name), false, false, '')::text, E'\n')
  FROM information_schema.tables
 WHERE table_type = 'BASE TABLE' AND table_schema NOT IN ('pg_catalog', 'information_schema')
   AND table_name <> 'chat_messages')sql") +
               ulw::test::scalar(conn, R"sql(
SELECT string_agg(concat_ws(' ', room_id, seq, sender, msg_key, sent_at, octet_length(body)),
                  E'\n')
  FROM chat_messages)sql");
    }

    // How many of the room's stored messages hold exactly `body`'s bytes.
    [[nodiscard]] std::string stored_exactly(const std::string& body) const {
        auto conn = db_->session();
        return ulw::test::scalar(
            conn, "SELECT count(*) FROM chat_messages WHERE room_id = $1::text::uuid AND body = $2",
            Params{}.add_text(room_).add_bytea(std::as_bytes(std::span{body})));
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
    // Each body the room stored is there once, as exactly the bytes sent: that is what the
    // search below covers. Everything else in the database, and every node's log, is searched
    // for each body as it was sent, as the client encoded it, and as bytea would show it.
    void expect_no_plaintext(const std::vector<std::string>& bodies,
                             const std::vector<std::string>& never_stored = {}) const {
        for (const std::string& body : bodies) {
            const bool refused = std::ranges::find(never_stored, body) != never_stored.end();
            EXPECT_EQ(stored_exactly(body), refused ? "0" : "1") << body;
        }
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
            std::cout << "each stored once as the bytes sent; searched " << database.size()
                      << " bytes of other rows and " << nodes_.size() << " logs for "
                      << bodies.size() << (bodies.size() == 1 ? " body" : " bodies")
                      << " in three forms: none found\n";
        }
    }

    os::SystemClock clock_;
    os::SystemRandom random_;
    std::optional<devtoken::DevKey> key_;
    std::unique_ptr<ScratchDatabase> db_;
    ulw::test::TempDir files_{"ulw-chat-cluster"};
    std::vector<std::string> tokens_;
    std::vector<Node> nodes_;
    std::string room_;
    std::string node_secret_;
    std::string jwks_;
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
    std::vector<std::string> refused;
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
            refused.push_back(bodies[i]);
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
    ASSERT_NO_FATAL_FAILURE(expect_no_plaintext(bodies, refused));
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

TEST_P(ChatClusterTest, AClientResumingThroughANodeThatKeptNothingFillsTheGapFromHistory) {
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
    // It fills the gap from history, which the store has whichever node the client is on.
    const auto filled = history(*elsewhere, std::format(R"(,"after":{})", *last));
    ASSERT_TRUE(filled);
    ASSERT_EQ(filled->size(), 3U);
    for (std::size_t i = 0; i < filled->size(); ++i) {
        EXPECT_EQ((*filled)[i].seq, *last + 1 + i);
        EXPECT_EQ((*filled)[i].body, missed[i]);
        EXPECT_EQ((*filled)[i].sender, "alice");
    }
    std::cout << "bob came back through chat-3 after seq " << *last
              << ", which kept nothing; joined named the head " << joined->seq
              << ", and history sent seqs " << filled->front().seq << ".." << filled->back().seq
              << "\n";
    missed.push_back(last_body_["alice"]);
    ASSERT_NO_FATAL_FAILURE(expect_no_plaintext(missed));
}

TEST_P(ChatClusterTest, HistorySurvivesARestartOfEveryNodeInTheOrderItWasSent) {
    auto alice = connect(nodes_[0], 0);
    auto bob = connect(nodes_[1], 1);
    ASSERT_TRUE(alice && bob);
    ASSERT_NO_FATAL_FAILURE(join(*alice));
    ASSERT_NO_FATAL_FAILURE(join(*bob));
    std::vector<std::string> sent;
    for (int i = 0; i < 6; ++i) {
        Client& from = i % 2 == 0 ? *alice : *bob;
        sent.push_back(std::format("kept across restarts {}", i));
        ASSERT_TRUE(from.send(send_command(room_, sent.back(), std::format("keep-{}", i))));
        ASSERT_TRUE(alice->message(sent.back()));
    }
    const std::vector<Seen> live = alice->messages();
    alice.reset();
    bob.reset();

    for (Node& n : nodes_) {
        n.process->signal(SIGTERM);
    }
    for (Node& n : nodes_) {
        ASSERT_EQ(n.process->wait_exit(seconds(30)), 0) << n.name << "\n" << n.process->output();
    }
    for (Node& n : nodes_) {
        ASSERT_NO_FATAL_FAILURE(start(n, jwks_));
    }
    ASSERT_NO_FATAL_FAILURE(wait_ready());

    auto carol = connect(nodes_[2], 2);
    ASSERT_TRUE(carol);
    ASSERT_NO_FATAL_FAILURE(join(*carol));
    const auto newest = history(*carol, R"(,"limit":4)");
    ASSERT_TRUE(newest);
    const auto oldest = history(*carol, std::format(R"(,"before":{})", newest->back().seq));
    ASSERT_TRUE(oldest);
    std::vector<Seen> all = *newest;
    all.insert(all.end(), oldest->begin(), oldest->end());
    ASSERT_EQ(all.size(), live.size());
    // Newest first from the store, as the live order reversed, byte for byte.
    for (std::size_t i = 0; i < all.size(); ++i) {
        const Seen& was = live[live.size() - 1 - i];
        EXPECT_EQ(all[i].seq, was.seq);
        EXPECT_EQ(all[i].id, was.id);
        EXPECT_EQ(all[i].sender, was.sender);
        EXPECT_EQ(all[i].body, was.body);
    }
    const auto past_the_start = history(*carol, std::format(R"(,"before":{})", live.front().seq));
    ASSERT_TRUE(past_the_start);
    EXPECT_TRUE(past_the_start->empty());
    std::cout << "after all three nodes restarted, history gave seqs " << all.front().seq << ".."
              << all.back().seq << " newest first, as they were delivered\n";
    ASSERT_NO_FATAL_FAILURE(expect_no_plaintext(sent));
}

TEST_P(ChatClusterTest, AResendAfterEveryNodeRestartedIsKnownToTheStoreAlone) {
    auto alice = connect(nodes_[0], 0);
    ASSERT_TRUE(alice);
    ASSERT_NO_FATAL_FAILURE(join(*alice));
    const std::string body = "sent before the restart";
    ASSERT_TRUE(alice->send(send_command(room_, body, "kept-key")));
    const auto ack =
        alice->wait_for([](const Seen& s) { return s.type == "sent" && s.id == "kept-key"; });
    ASSERT_TRUE(ack);
    alice.reset();
    // Every node's memory of recent keys goes with it, and the room gets a new owner.
    for (Node& n : nodes_) {
        n.process->signal(SIGTERM);
    }
    for (Node& n : nodes_) {
        ASSERT_EQ(n.process->wait_exit(seconds(30)), 0) << n.name << "\n" << n.process->output();
    }
    for (Node& n : nodes_) {
        ASSERT_NO_FATAL_FAILURE(start(n, jwks_));
    }
    ASSERT_NO_FATAL_FAILURE(wait_ready());

    auto again = connect(nodes_[1], 0);
    ASSERT_TRUE(again);
    ASSERT_NO_FATAL_FAILURE(join(*again));
    ASSERT_TRUE(again->send(send_command(room_, body, "kept-key")));
    const auto repeat =
        again->wait_for([](const Seen& s) { return s.type == "sent" && s.id == "kept-key"; });
    ASSERT_TRUE(repeat);
    EXPECT_EQ(repeat->seq, ack->seq);
    ASSERT_TRUE(again->send(send_command(room_, "another body", "kept-key")));
    const auto conflict =
        again->wait_for([](const Seen& s) { return s.type == "error" && s.id == "kept-key"; });
    ASSERT_TRUE(conflict);
    EXPECT_EQ(conflict->reason, "conflict");
    ASSERT_TRUE(again->send(send_command(room_, "after the resends", "marker")));
    ASSERT_TRUE(again->message("after the resends"));
    // Never sequenced again: the marker is the next seq, and nothing arrived under the key
    // but the first message, under its own seq.
    EXPECT_EQ(last_seq(), std::to_string(ack->seq + 1));
    for (const Seen& s : again->messages()) {
        EXPECT_TRUE(s.id != "kept-key" || (s.seq == ack->seq && s.body == body));
    }
    EXPECT_FALSE(again->ever_saw("another body"));
    for (const Node& n : nodes_) {
        EXPECT_EQ(metric(n, "messages_deduplicated_total"), 0U) << n.name << " answered it";
    }
    std::cout << "after every node restarted, the resend got seq " << repeat->seq
              << " from the store, and another body under its id was a conflict\n";
    ASSERT_NO_FATAL_FAILURE(expect_no_plaintext({body, "another body"}, {"another body"}));
}

TEST_P(ChatClusterTest, ARoomWithMembersRefusesEveryoneElse) {
    const std::string members_only = core::RoomId::generate(clock_, random_).to_string();
    ASSERT_NO_FATAL_FAILURE(list_members(members_only, {"alice", "bob"}));
    auto alice = connect(nodes_[0], 0);
    auto carol = connect(nodes_[2], 2);
    ASSERT_TRUE(alice && carol);
    EXPECT_EQ(join_answer(*alice, members_only), "joined");
    EXPECT_EQ(join_answer(*carol, members_only), "not_member");
    ASSERT_TRUE(carol->send(send_command(members_only, "let me in", "sneak")));
    const auto sneak = carol->wait_for([](const Seen& s) { return s.id == "sneak"; });
    ASSERT_TRUE(sneak);
    EXPECT_EQ(sneak->reason, "not_joined");
    ASSERT_TRUE(alice->send(send_command(members_only, "members only", "inside")));
    ASSERT_TRUE(alice->message("members only"));
    EXPECT_FALSE(carol->ever_saw("members only"));
}

TEST_P(ChatClusterTest, AGroupRoomWithNoMembersRefusesEveryoneAndCannotBeOpenedLater) {
    const std::string nobody = core::RoomId::generate(clock_, random_).to_string();
    auto alice = connect(nodes_[0], 0);
    auto bob = connect(nodes_[1], 1);
    ASSERT_TRUE(alice && bob);
    EXPECT_EQ(join_answer(*alice, nobody), "not_member");
    // Its first join recorded it as a group chat. A room id cannot ask to be a live chat (only a
    // stream's room is one, ADR-0057), and the database refuses to record it as one.
    EXPECT_EQ(join_answer(*bob, nobody, R"(,"kind":"live")"), "malformed");
    auto conn = db_->session();
    EXPECT_FALSE(
        conn.exec("UPDATE chat_rooms SET kind = 'stream_live_chat' WHERE room_id = $1::text::uuid",
                  Params{}.add_text(nobody)));
    EXPECT_EQ(ulw::test::scalar(conn, "SELECT kind FROM chat_rooms WHERE room_id = $1::text::uuid",
                                Params{}.add_text(nobody)),
              "group_chat");
}

TEST_P(ChatClusterTest, AStreamsChatRefusesViewersUntilTheServerOpensItAndRecordsNothing) {
    const std::string stream = "unopened-" + room_.substr(0, 8);
    auto alice = connect(nodes_[0], 0);
    ASSERT_TRUE(alice);
    EXPECT_EQ(stream_join_answer(*alice, stream), "not_live");
    auto conn = db_->session();
    EXPECT_EQ(ulw::test::scalar(
                  conn, "SELECT count(*) FROM chat_rooms WHERE room_id = live_chat_room($1)",
                  Params{}.add_text(stream)),
              "0");
}

TEST_P(ChatClusterTest, AnOpenedStreamsChatAdmitsAnyoneByTheStreamsName) {
    const std::string stream = "open-" + room_.substr(0, 8);
    ASSERT_EQ(record_live(stream), "stream_live_chat");
    auto alice = connect(nodes_[0], 0);
    auto carol = connect(nodes_[2], 2);
    ASSERT_TRUE(alice && carol);
    EXPECT_EQ(stream_join_answer(*alice, stream), "joined");
    EXPECT_EQ(stream_join_answer(*carol, stream), "joined");
    const auto joined = alice->wait_for([](const Seen& s) { return s.type == "joined"; });
    ASSERT_TRUE(joined);
    const std::string live = joined->room;
    // Named by its id, it is not joined: a stream's room is reached only by the stream.
    auto bob = connect(nodes_[1], 1);
    ASSERT_TRUE(bob);
    EXPECT_EQ(join_answer(*bob, live), "bad_room");
    ASSERT_TRUE(carol->send(send_command(live, "hello, stream", "live-1")));
    ASSERT_TRUE(alice->message("hello, stream"));
}

#if defined(__SANITIZE_ADDRESS__)
constexpr bool kAddressSanitizer = true;
#elif defined(__has_feature)
constexpr bool kAddressSanitizer = __has_feature(address_sanitizer);
#else
constexpr bool kAddressSanitizer = false;
#endif

// Resident memory, from /proc: what the node holds, the kernel's socket buffers aside.
std::uint64_t resident_kib(pid_t pid) {
    std::ifstream status("/proc/" + std::to_string(pid) + "/status");
    std::string line;
    while (std::getline(status, line)) {
        if (line.starts_with("VmRSS:")) {
            const std::string_view rest = std::string_view(line).substr(6);
            const std::size_t digits = rest.find_first_of("0123456789");
            const std::size_t end = rest.find(' ', digits);
            return core::parse_integer<std::uint64_t>(rest.substr(digits, end - digits))
                .value_or(0);
        }
    }
    return 0;
}

TEST_P(ChatClusterTest, SlowViewersCostTheirNodeNoMemoryAndOthersMissNothing) {
    const std::string stream = "m32-" + room_.substr(0, 8);
    ASSERT_EQ(record_live(stream), "stream_live_chat");
    const auto join_live = [&](Client& c) {
        EXPECT_TRUE(c.send(R"({"type":"join","stream":")" + stream + R"("})"));
        const auto joined =
            c.wait_for([](const Seen& s) { return s.type == "joined" || s.type == "error"; });
        EXPECT_TRUE(joined && joined->type == "joined") << c.name();
        return joined ? joined->room : std::string();
    };
    // Ten senders on each node, each a viewer too, and two viewers that only read. Eight slow
    // viewers are on chat-2, each with its receive buffer fixed at 64 KiB (128 KiB in the
    // kernel). Every 90 messages sequenced, once chat-2's lossy_drops_total shows they are
    // behind, each reads 64 KiB, a quarter of what those messages send it. None stops outright:
    // the kernel ends a connection whose window stays shut for TCP_USER_TIMEOUT (20 s), longer
    // than 90 messages take even under a sanitizer, and a 64 KiB read frees half the buffer,
    // which reopens the window.
    std::vector<std::unique_ptr<Client>> senders;
    std::vector<std::unique_ptr<Client>> viewers;
    std::string live;
    for (std::size_t n = 0; n < nodes_.size(); ++n) {
        for (int k = 0; k < 10; ++k) {
            senders.push_back(connect_as(nodes_[n], std::format("sender-{}-{}", n, k)));
            ASSERT_TRUE(senders.back());
            live = join_live(*senders.back());
        }
        for (int k = 0; k < 2; ++k) {
            viewers.push_back(connect_as(nodes_[n], std::format("viewer-{}-{}", n, k)));
            ASSERT_TRUE(viewers.back());
            join_live(*viewers.back());
        }
    }
    constexpr int kSlowReceiveBuffer = 64 * 1024;
    constexpr std::size_t kReadEvery = 90;
    constexpr std::size_t kSlowRead = std::size_t{64} * 1024;
    Node& slow_node = nodes_[1];
    std::vector<std::unique_ptr<Client>> slow;
    for (int k = 0; k < 8; ++k) {
        slow.push_back(connect_as(slow_node, std::format("slow-viewer-{}", k), kSlowReceiveBuffer));
        ASSERT_TRUE(slow.back());
        ASSERT_EQ(join_live(*slow.back()), live);
    }
    ASSERT_FALSE(HasFailure());

    // 900 messages of 2000 bytes, about 2.6 MiB for each viewer: rounds of one message per
    // sender, each round read by everyone but the slow viewers before the next, so that no
    // one else is ever behind. A send the room or the user turns away as rate_limited is tried
    // again in the next round, with a new id since it was never sequenced; the room's allowance
    // (40, then 20 a second on each node) sets the pace.
    constexpr std::size_t kMessages = 900;
    // Past the first half, the room keeps its most and the slow viewers are long behind.
    constexpr std::size_t kSettled = 450;
    const auto body = [](std::size_t k) {
        std::string b = std::format("live {} ", k);
        b.resize(2'000, '.');
        return b;
    };
    std::vector<std::optional<std::size_t>> holding(senders.size());
    std::vector<std::string> ids(senders.size());
    std::size_t next = 0;
    std::size_t acked = 0;
    std::size_t attempts = 0;
    std::size_t reads = 0;
    std::uint64_t drops_at_read = 0;
    std::vector<std::size_t> read_bytes(slow.size());
    std::uint64_t head = 0;
    std::uint64_t settled_kib = 0;
    std::uint64_t settled_seq = 0;
    while (acked < kMessages) {
        // A node that turned one send away turns away the rest of the round's too: they are
        // not tried, which keeps the retrying down to a few sends a round.
        std::array<bool, 3> refused{};
        for (std::size_t s = 0; s < senders.size(); ++s) {
            const std::size_t node = s / 10;
            if (refused.at(node)) {
                continue;
            }
            if (!holding[s] && next < kMessages) {
                holding[s] = next++;
            }
            if (!holding[s]) {
                continue;
            }
            // unavailable leaves the send's fate unknown: it goes again under the same id, and
            // is sequenced once (ADR-0043). A send turned away was never sequenced.
            if (ids[s].empty()) {
                ids[s] = std::format("m{}-{}", *holding[s], attempts);
            }
            ++attempts;
            ASSERT_TRUE(senders[s]->send(send_command(live, body(*holding[s]), ids[s])));
            const std::size_t answers = senders[s]->count([&](const Seen& seen) {
                return (seen.type == "sent" || seen.type == "error") && seen.id == ids[s];
            });
            ASSERT_TRUE(senders[s]->wait_for([&](const Seen&) {
                return senders[s]->count([&](const Seen& seen) {
                    return (seen.type == "sent" || seen.type == "error") && seen.id == ids[s];
                }) > answers;
            })) << ids[s];
            const Seen answer =
                *std::ranges::find_last_if(senders[s]->seen(), [&](const Seen& seen) {
                     return (seen.type == "sent" || seen.type == "error") && seen.id == ids[s];
                 }).begin();
            if (answer.type == "sent") {
                head = std::max(head, answer.seq);
                holding[s].reset();
                ids[s].clear();
                ++acked;
            } else if (answer.reason == "rate_limited") {
                refused.at(node) = true;
                ids[s].clear();
            } else {
                ASSERT_EQ(answer.reason, "unavailable") << ids[s];
            }
        }
        ASSERT_LT(attempts, 200'000U) << "the rate limit never lifted";
        for (auto* group : {&senders, &viewers}) {
            for (auto& c : *group) {
                ASSERT_TRUE(c->wait_for([&](const Seen& seen) {
                    return seen.type == "message" && seen.seq >= head;
                })) << c->name()
                    << " never got seq " << head;
            }
        }
        if (acked / kReadEvery > reads) {
            reads = acked / kReadEvery;
            if (const std::uint64_t drops = metric(slow_node, "lossy_drops_total");
                drops > drops_at_read) {
                drops_at_read = drops;
                for (std::size_t k = 0; k < slow.size(); ++k) {
                    read_bytes[k] += slow[k]->trickle(kSlowRead);
                }
            }
        }
        if (settled_kib == 0 && acked >= kSettled) {
            settled_kib = resident_kib(slow_node.process->pid());
            settled_seq = head;
        }
    }
    const std::uint64_t final_kib = resident_kib(slow_node.process->pid());

    // Everyone who kept reading got every message, once, in the room's order.
    for (auto* group : {&senders, &viewers}) {
        for (auto& c : *group) {
            const std::vector<Seen> heard = c->messages();
            ASSERT_EQ(heard.size(), kMessages) << c->name();
            for (std::size_t k = 0; k < heard.size(); ++k) {
                ASSERT_EQ(heard[k].seq, k + 1) << c->name();
            }
        }
    }
    // What the slow viewers' node held for them did not grow with what they were sent. Past
    // the settled point each can be queued at most lossy_backlog and one message (67 KiB); the
    // room's kept messages (256 KiB) were full long before; each message sequenced adds one
    // remembered key (ADR-0043), about 300 bytes; and the allocator gets 1 MiB for arenas and
    // fragments of its own. Nothing there grows with how far behind a viewer is, where an
    // unbounded queue would hold each one's 1.3 MiB more. AddressSanitizer keeps freed memory in
    // its quarantine instead of returning it, so its resident memory grows with every free,
    // bounded or not: under it the numbers are only reported.
    const std::uint64_t allowed_kib =
        (slow.size() * 67) + ((kMessages - settled_seq) * 300 / 1024) + 1024;
    const std::uint64_t owed_kib = slow.size() * (kMessages - settled_seq) * 2'700 / 1024;
    EXPECT_TRUE(kAddressSanitizer || final_kib - std::min(final_kib, settled_kib) < allowed_kib)
        << "resident " << settled_kib << " KiB at seq " << settled_seq << ", " << final_kib
        << " KiB at seq " << kMessages << ", allowed " << allowed_kib << " KiB more";

    // Reading in full, each slow viewer gets the rest of what it was owed, in order, up to the
    // last message, after gaps where it dropped the rest, which its node counted exactly.
    std::uint64_t missed = 0;
    std::size_t fewest = kMessages;
    for (auto& c : slow) {
        ASSERT_TRUE(c->wait_for([&](const Seen& seen) {
            return seen.type == "message" && seen.seq == kMessages;
        })) << c->name()
            << " stopped after " << c->messages().size() << " messages, at seq "
            << (c->messages().empty() ? 0 : c->messages().back().seq);
        const std::vector<Seen> got = c->messages();
        for (std::size_t k = 1; k < got.size(); ++k) {
            ASSERT_LT(got[k - 1].seq, got[k].seq) << c->name();
        }
        // A message is at least its body in base64url (2667 bytes) on the wire. A slow viewer was
        // sent only what it read, what its node's send buffer (64 KiB), its own receive buffer
        // (128 KiB) and its queue before it counts as behind (lossy_backlog, 64 KiB, and one
        // message of at most 2987 bytes) held at the end, and the newest 64 it was owed then.
        const auto k = static_cast<std::size_t>(&c - slow.data());
        const std::size_t bound =
            ((read_bytes[k] + (std::size_t{256} * 1024) + 2'987) / 2'667) + 64;
        EXPECT_LE(got.size(), bound) << c->name() << " read " << read_bytes[k] << " bytes";
        missed += kMessages - got.size();
        fewest = std::min(fewest, got.size());
    }
    EXPECT_GT(missed, 0U);
    EXPECT_EQ(metric(slow_node, "lossy_drops_total"), missed);
    for (std::size_t n = 0; n < nodes_.size(); ++n) {
        if (n != 1) {
            EXPECT_EQ(metric(nodes_[n], "lossy_drops_total"), 0U) << nodes_[n].name;
        }
    }
    std::cout << senders.size() + viewers.size() << " viewers on " << nodes_.size()
              << " nodes got all " << kMessages << " messages in order; " << slow.size()
              << " slow ones got as few as " << fewest << ", and their node dropped " << missed
              << " for them; it was resident at " << settled_kib << " KiB at seq " << settled_seq
              << " and " << final_kib << " KiB at the end (allowed " << allowed_kib
              << " more), having sent them " << owed_kib << " KiB more; " << attempts - kMessages
              << " sends were turned away or unanswered and tried again\n";
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
