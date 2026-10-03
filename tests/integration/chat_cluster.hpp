// Three chat_server processes on a scratch Postgres, and clients of theirs: what the M16 to M19
// and M32 acceptance (chat_cluster_test.cpp) and the E2EE checkpoint (chat_e2ee_test.cpp) run on.
// ULW_CHAT_CLUSTER_PORTS=9101,9102,9103 pins the client ports (the CI job does); otherwise
// ones outside the ephemeral range are reserved.
#pragma once

#include "core/util/json.hpp"
#include "core/util/parse.hpp"
#include "infra/auth/base64url.hpp"
#include "os/system_clock.hpp"
#include "os/system_random.hpp"

#include "devtoken/dev_key.hpp"
#include "postgres_harness.hpp"
#include "support/child_process.hpp"
#include "support/core_limit.hpp"
#include "support/reactor_harness.hpp"
#include "support/reserve_port.hpp"
#include "support/temp_dir.hpp"
#include "support/ws_client.hpp"

#include <algorithm>
#include <array>
#include <chrono>
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

namespace ulw::test {

inline constexpr std::string_view kIssuer = "https://auth.example.com";
inline constexpr std::chrono::milliseconds kReadyCheckPeriod{250};
inline constexpr std::array kUsers{"alice", "bob", "carol", "dave"};
// Short, so that the presence tests wait seconds for a grace to run out, not the default ten.
inline constexpr std::chrono::milliseconds kGrace{2'000};
// Short, so that the call test waits seconds for a ring nobody answers to run out.
inline constexpr std::chrono::milliseconds kRingTimeout{3'000};

// The LiveKit server calls go to, as tests/call/run.sh names it: LIVEKIT_API_URL,
// LIVEKIT_CLIENT_URL, LIVEKIT_API_KEY and LIVEKIT_API_SECRET, all four or none. With them the
// nodes answer calls (ADR-0050); without them they answer calls_disabled.
inline std::vector<std::string> livekit_environment() {
    std::vector<std::string> out;
    for (const char* name :
         {"LIVEKIT_API_URL", "LIVEKIT_CLIENT_URL", "LIVEKIT_API_KEY", "LIVEKIT_API_SECRET"}) {
        // NOLINTNEXTLINE(concurrency-mt-unsafe): read before any thread starts.
        const char* value = std::getenv(name);
        if (value == nullptr || *value == '\0') {
            return {};
        }
        out.push_back(std::string(name) + "=" + value);
    }
    return out;
}

// Empty when the ports are not pinned: each node then reserves its own.
inline std::vector<std::uint16_t> pinned_client_ports() {
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
    std::string user;
    std::string status;
    // Of a call's ticket.
    std::string url;
    std::string token;
    std::uint64_t expires_at = 0;
    // Of a ticket and a call's events (ADR-0091).
    std::string call;
    std::string from;
    std::string by;
};

inline std::optional<Seen> parse_seen(const std::string& text) {
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
           .count = 0,
           .user = string("user"),
           .status = string("status"),
           .url = string("url"),
           .token = string("token"),
           .expires_at = 0,
           .call = string("call"),
           .from = string("from"),
           .by = string("by")};
    if (const core::json::Value* expires = json->find("expires_at")) {
        s.expires_at = expires->as_u64().value_or(0);
    }
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
    // Reads at most `bytes` of what has arrived, without waiting, and takes in every whole
    // message among it, answering pings as any client that reads them does: a slow reader, not
    // one that stopped. Returns how many bytes it read.
    std::size_t trickle(std::size_t bytes) {
        const std::size_t read = ws_.read_at_most(bytes);
        while (const auto text = ws_.next_text(std::chrono::milliseconds{0})) {
            if (!keep(*text)) {
                break;
            }
        }
        return read;
    }
    // How its connection ended, for a failure's message: "open" while it has not.
    [[nodiscard]] std::string ending() const {
        if (ws_.connected()) {
            return "open";
        }
        return ws_.error() == 0 ? "closed" : std::format("ended, errno {}", ws_.error());
    }

    // Reads until a message matching `pred` arrives, keeping everything read.
    template <class Pred>
    std::optional<Seen> wait_for(Pred pred,
                                 std::chrono::milliseconds limit = std::chrono::seconds(15)) {
        const auto at = wait_from(0, pred, limit);
        if (!at) {
            return std::nullopt;
        }
        return seen_[*at];
    }

    // Where in seen() the first message from `from` on that matches `pred` is, reading until
    // one arrives. Each message is looked at once: a caller that waits again from what it was
    // given, or from what it had before it sent, costs what is new, not all it has heard.
    template <class Pred>
    std::optional<std::size_t>
    wait_from(std::size_t from, Pred pred,
              std::chrono::milliseconds limit = std::chrono::seconds(15)) {
        for (std::size_t i = from; i < seen_.size(); ++i) {
            if (pred(seen_[i])) {
                return i;
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
            if (!keep(*text)) {
                return std::nullopt;
            }
            if (seen_.size() > from && pred(seen_.back())) {
                return seen_.size() - 1;
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
    // Keeps a server message; false, after a failure, for anything that is not JSON.
    bool keep(const std::string& text) {
        auto s = parse_seen(text);
        if (!s) {
            ADD_FAILURE() << name_ << " got something that is not JSON: " << text;
            return false;
        }
        seen_.push_back(std::move(*s));
        return true;
    }

    WsClient ws_;
    std::string name_;
    std::vector<Seen> seen_;
};

// A body as it was sent, as the client encoded it, and as bytea would show it.
inline std::array<std::string, 3> readable_forms(const std::string& text) {
    std::string hex;
    for (const char c : text) {
        hex += std::format("{:02x}", static_cast<unsigned char>(c));
    }
    return {text, infra::auth::encode_base64url(text), hex};
}

struct Node {
    std::string name;
    std::uint16_t port = 0;
    bool port_pinned = false;
    std::uint16_t node_port = 0;
    std::unique_ptr<ChildProcess> process;
};

class ChatCluster : public ::testing::TestWithParam<net::ReactorKind> {
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
        for (const char* user : kUsers) {
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
            ASSERT_TRUE(
                n.process->poll_until([&] { return http_get(n.port, "/readyz").status == 200; },
                                      std::chrono::seconds(30), kReadyCheckPeriod))
                << n.process->output();
        }
    }

    void start(Node& node, const std::string& jwks) {
        std::vector<std::string> env{
            "ULW_NODE_ID=" + node.name, "ULW_DEV_LOOPBACK_NODES=1",
            "ULW_NODE_SECRET=" + node_secret_, "ULW_DATABASE_URL=" + db_->conninfo(),
            "ULW_DEV_JWKS_FILE=" + jwks, "ULW_DEV_MODE=1", "JWT_ISSUER=" + std::string(kIssuer),
            "ULW_PRESENCE_GRACE_MS=" + std::to_string(kGrace.count()),
            "ULW_CALL_RING_TIMEOUT_MS=" + std::to_string(kRingTimeout.count()),
            "ULW_REACTOR=" +
                std::string(GetParam() == net::ReactorKind::IoUring ? "io_uring" : "epoll"),
            // Some runs start tests as root; this suite is not about that.
            "ULW_ALLOW_ROOT=1",
            // Every client here, and every readiness poll, comes from 127.0.0.1; the per-address
            // limits have tests of their own (tests/unit/chat/session_test.cpp).
            "ULW_MAX_CONNECTIONS_PER_IP=1280", "ULW_NEW_CONNECTIONS_PER_IP_PER_SECOND=65536"};
        for (std::string& livekit : livekit_environment()) {
            env.push_back(std::move(livekit));
        }
        for (const char* passed : {"ASAN_OPTIONS", "UBSAN_OPTIONS", "LSAN_OPTIONS"}) {
            // NOLINTNEXTLINE(concurrency-mt-unsafe): read before any thread starts.
            if (const char* value = std::getenv(passed)) {
                env.push_back(std::string(passed) + "=" + value);
            }
        }
        auto started = start_until_listening(
            [&] {
                if (!node.port_pinned) {
                    node.port = reserve_port();
                }
                node.node_port = reserve_port();
                if (node.port == 0 || node.node_port == 0) {
                    return std::unique_ptr<ChildProcess>();
                }
                auto with_ports = env;
                with_ports.push_back("ULW_LISTEN_PORT=" + std::to_string(node.port));
                with_ports.push_back("ULW_NODE_ADDRESS=127.0.0.1:" +
                                     std::to_string(node.node_port));
                return ChildProcess::start({ULW_CHAT_BIN}, with_ports);
            },
            R"("msg":"listening")", std::chrono::seconds(30));
        node.process = std::move(started.process);
        ASSERT_NE(node.process, nullptr) << "no port to listen on";
        ASSERT_TRUE(started.ready) << node.process->output();
    }

    [[nodiscard]] std::string mint(const std::string& user) const {
        return key_
            ->mint({.issuer = std::string(kIssuer),
                    .audience = "ulw-dev",
                    .subject = user,
                    .email = {},
                    .ttl = std::chrono::seconds(600)},
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
        return std::make_unique<Client>(std::move(*ws), kUsers.at(user));
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
    void list_members(const std::string& room, const std::vector<std::string>& users,
                      const std::string& kind = "group_chat") const {
        auto conn = db_->session();
        ASSERT_TRUE(conn.exec("INSERT INTO chat_rooms (room_id, kind) "
                              "VALUES ($1::text::uuid, $2) "
                              "ON CONFLICT (room_id) DO NOTHING",
                              infra::postgres::Params{}.add_text(room).add_text(kind)));
        for (const std::string& user : users) {
            ASSERT_TRUE(
                conn.exec("INSERT INTO chat_members (room_id, user_id) VALUES ($1::text::uuid, $2)",
                          infra::postgres::Params{}.add_text(room).add_text(user)));
        }
    }

    // Opens a stream's live chat, as the server side does (RUNBOOK section 3); the kind its
    // room is then recorded as.
    [[nodiscard]] std::string record_live(const std::string& stream) const {
        auto conn = db_->session();
        return scalar(conn,
                      "INSERT INTO chat_rooms (room_id, kind) "
                      "SELECT live_chat_room($1), 'stream_live_chat' WHERE NOT EXISTS "
                      "(SELECT 1 FROM chat_members WHERE room_id = live_chat_room($1)) "
                      "AND NOT EXISTS (SELECT 1 FROM room_state WHERE room_id = live_chat_room($1) "
                      "AND kind <> 'stream_live_chat') "
                      "ON CONFLICT (room_id) DO UPDATE SET kind = chat_rooms.kind RETURNING kind",
                      infra::postgres::Params{}.add_text(stream));
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
        return scalar(conn,
                      "SELECT owner_node || ' ' || owner_generation "
                      "FROM room_assignments WHERE room_id = $1::text::uuid",
                      infra::postgres::Params{}.add_text(room_));
    }

    [[nodiscard]] std::string last_seq() const {
        auto conn = db_->session();
        return scalar(conn, "SELECT last_seq FROM room_state WHERE room_id = $1::text::uuid",
                      infra::postgres::Params{}.add_text(room_));
    }

    // Every row of every table, as text, but for the bytes of stored bodies: those are the
    // messages themselves, and are checked apart (stored_exactly). Their length stands in.
    [[nodiscard]] std::string database_text() const {
        auto conn = db_->session();
        return scalar(conn, R"sql(
SELECT string_agg(query_to_xml(format('SELECT t::text AS row FROM %I.%I t', table_schema,
                                      table_name), false, false, '')::text, E'\n')
  FROM information_schema.tables
 WHERE table_type = 'BASE TABLE' AND table_schema NOT IN ('pg_catalog', 'information_schema')
   AND table_name <> 'chat_messages')sql") +
               scalar(conn, R"sql(
SELECT string_agg(concat_ws(' ', room_id, seq, sender, msg_key, sent_at, octet_length(body)),
                  E'\n')
  FROM chat_messages)sql");
    }

    // How many of the room's stored messages hold exactly `body`'s bytes.
    [[nodiscard]] std::string stored_exactly(const std::string& body) const {
        auto conn = db_->session();
        return scalar(
            conn, "SELECT count(*) FROM chat_messages WHERE room_id = $1::text::uuid AND body = $2",
            infra::postgres::Params{}.add_text(room_).add_bytea(std::as_bytes(std::span{body})));
    }

    [[nodiscard]] static std::uint64_t metric(const Node& node, std::string_view name) {
        const std::string body = http_get(node.port, "/metrics").body;
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
        const std::size_t searched = expect_in_no_row_or_log(bodies);
        if (!HasFailure()) {
            std::cout << "each stored once as the bytes sent; searched " << searched
                      << " bytes of other rows and " << nodes_.size() << " logs for "
                      << bodies.size() << (bodies.size() == 1 ? " body" : " bodies")
                      << " in three forms: none found\n";
        }
    }

    // Every row but the stored bodies, and every node's log, searched for each of `texts` in
    // each of its readable_forms. Returns how many bytes of rows were searched.
    std::size_t expect_in_no_row_or_log(const std::vector<std::string>& texts) const {
        const std::string database = database_text();
        EXPECT_NE(database.find(room_), std::string::npos) << "no rows read";
        for (const std::string& text : texts) {
            for (const std::string& form : readable_forms(text)) {
                EXPECT_EQ(database.find(form), std::string::npos) << "the database holds " << form;
                for (const Node& n : nodes_) {
                    EXPECT_EQ(n.process->output().find(form), std::string::npos)
                        << n.name << " logged " << form;
                }
            }
        }
        return database.size();
    }

    os::SystemClock clock_;
    os::SystemRandom random_;
    std::optional<devtoken::DevKey> key_;
    std::unique_ptr<ScratchDatabase> db_;
    TempDir files_{"ulw-chat-cluster"};
    std::vector<std::string> tokens_;
    std::vector<Node> nodes_;
    std::string room_;
    std::string node_secret_;
    std::string jwks_;
    std::uint64_t next_ref_ = 100;
    std::unordered_map<std::string, std::string> last_body_;
};

} // namespace ulw::test
